/*!
 *  Copyright (c) 2023 by Contributors
 * \file serve/sampler/gpu_sampler.cc
 * \brief The implementation for GPU sampler functions.
 */
#include <tvm/runtime/device_api.h>
#include <tvm/runtime/ndarray.h>
#include <tvm/runtime/nvtx.h>
#include <tvm/runtime/packed_func.h>

#include "../../random.h"
#include "sampler.h"

namespace mlc {
namespace llm {
namespace serve {

inline void CopyArray(NDArray src, NDArray dst, TVMStreamHandle copy_stream) {
  DLTensor dl_dst = *(dst.operator->());
  NDArray::CopyFromTo(src.operator->(), &dl_dst, copy_stream);
}

inline void SyncCopyStream(Device device, TVMStreamHandle compute_stream,
                           TVMStreamHandle copy_stream) {
  // - If there is no particular copy stream, no action is needed.
  if (copy_stream == nullptr) {
    return;
  }
  // - Sync two streams.
  DeviceAPI::Get(device)->SyncStreamFromTo(device, copy_stream, compute_stream);
}

/*********************** GPU Sampler ***********************/

class GPUSampler : public SamplerObj {
 public:
  explicit GPUSampler(int max_num_sample, int vocab_size, FunctionTable* ft, DLDevice device,
                      Optional<EventTraceRecorder> trace_recorder)
      : max_num_sample_(max_num_sample),
        vocab_size_(vocab_size),
        device_(device),
        gpu_multinomial_from_uniform_func_(ft->gpu_multinomial_from_uniform_func_),
        gpu_argsort_probs_func_(ft->gpu_argsort_probs_func_),
        gpu_sample_with_top_p_func_(ft->gpu_sample_with_top_p_func_),
        gpu_sampler_take_probs_func_(ft->gpu_sampler_take_probs_func_),
        trace_recorder_(std::move(trace_recorder)) {
    ICHECK(gpu_multinomial_from_uniform_func_.defined());
    ICHECK(gpu_argsort_probs_func_.defined());
    ICHECK(gpu_sample_with_top_p_func_.defined());
    ICHECK(gpu_sampler_take_probs_func_.defined());

    DLDevice device_cpu{DLDeviceType::kDLCPU, /*device_id=*/0};
    // We support at most 5 top prob results for each sequence.
    // Initialize auxiliary arrays on CPU.
    uniform_samples_host_ = NDArray::Empty({max_num_sample}, dtype_f32_, device_cpu);
    sample_indices_host_ = NDArray::Empty({max_num_sample}, dtype_i32_, device_cpu);
    top_p_host_ = NDArray::Empty({max_num_sample}, dtype_f32_, device_cpu);
    top_prob_offsets_host_ = NDArray::Empty({max_num_sample * 5}, dtype_i32_, device_cpu);
    sampled_token_ids_host_ = NDArray::Empty({max_num_sample}, dtype_i32_, device_cpu);
    sampled_probs_host_ = NDArray::Empty({max_num_sample}, dtype_f32_, device_cpu);
    top_prob_probs_host_ = NDArray::Empty({max_num_sample * 5}, dtype_f32_, device_cpu);
    top_prob_indices_host_ = NDArray::Empty({max_num_sample * 5}, dtype_i32_, device_cpu);
    // Initialize auxiliary arrays on GPU.
    uniform_samples_device_ = NDArray::Empty({max_num_sample}, dtype_f32_, device);
    sample_indices_device_ = NDArray::Empty({max_num_sample}, dtype_i32_, device);
    top_p_device_ = NDArray::Empty({max_num_sample}, dtype_f32_, device);
    top_prob_offsets_device_ = NDArray::Empty({max_num_sample * 5}, dtype_i32_, device);

    // If the device is CUDA/ROCm, we create a standalone copy stream, in
    // purpose to hide the latency of auxiliary stream copy.
    if (device.device_type == DLDeviceType::kDLCUDA ||
        device.device_type == DLDeviceType::kDLROCM) {
      // The compute stream is the default stream.
      compute_stream_ = DeviceAPI::Get(device)->GetCurrentStream(device);
      copy_stream_ = DeviceAPI::Get(device)->CreateStream(device);
    }
  }

  ~GPUSampler() {
    // Free the copy stream if defined.
    if (copy_stream_ != nullptr) {
      DeviceAPI::Get(device_)->FreeStream(device_, copy_stream_);
    }
  }

  std::vector<SampleResult> BatchSampleTokens(NDArray probs_on_device,                        //
                                              const std::vector<int>& sample_indices,         //
                                              const Array<String>& request_ids,               //
                                              const Array<GenerationConfig>& generation_cfg,  //
                                              const std::vector<RandomGenerator*>& rngs,      //
                                              std::vector<NDArray>* output_prob_dist) final {
    NVTXScopedRange nvtx_scope("BatchSampleTokens");
    // probs_on_device: (n, v)
    RECORD_EVENT(trace_recorder_, request_ids, "start sampling");
    CHECK_EQ(probs_on_device->ndim, 2);
    int num_samples = sample_indices.size();
    int num_probs = probs_on_device->shape[0];
    int vocab_size = probs_on_device->shape[1];
    ICHECK_EQ(request_ids.size(), num_samples);
    ICHECK_EQ(generation_cfg.size(), num_samples);
    ICHECK_EQ(rngs.size(), num_samples);

    // - Generate random numbers.
    //   Copy the random numbers and sample indices.
    auto [uniform_samples_device, sample_indices_device] =
        CopySamplesAndIndicesToGPU(sample_indices, rngs, num_samples);

    // - Check if there is need for applying top p or prob values,
    //   so that argsort is needed.
    bool need_top_p = false;
    bool need_prob_values = false;
    // The indptr array of the number of top probs for each sample.
    std::vector<int> top_prob_offset_indptr;
    CheckTopPAndProbValues(generation_cfg, sample_indices, num_probs, num_samples, vocab_size,
                           &need_top_p, &need_prob_values, &top_prob_offset_indptr);

    // - Sample tokens on GPU, and take out the probability values if needed.
    std::vector<NDArray> device_arrays =
        SampleOnGPU(probs_on_device, uniform_samples_device, sample_indices_device, need_top_p,
                    need_prob_values, num_probs, top_prob_offset_indptr);

    // - Copy the GPU sampling function results to CPU.
    std::vector<NDArray> host_arrays = CopyArraysToCPU(device_arrays, num_samples, need_prob_values,
                                                       top_prob_offset_indptr.back());

    // - Collect the sampling results.
    const int* p_sampled_token_ids = static_cast<const int*>(host_arrays[0]->data);
    const float* p_sampled_probs = nullptr;
    const float* p_top_prob_probs = nullptr;
    const int* p_top_prob_indices = nullptr;
    if (need_prob_values) {
      p_sampled_probs = static_cast<const float*>(host_arrays[1]->data);
      p_top_prob_probs = static_cast<const float*>(host_arrays[2]->data);
      p_top_prob_indices = static_cast<const int*>(host_arrays[3]->data);
    }
    std::vector<SampleResult> sample_results;
    sample_results.reserve(num_samples);
    ICHECK_EQ(top_prob_offset_indptr.size(), num_samples + 1);
    for (int i = 0; i < num_samples; ++i) {
      // Note: we set the probability in SampleResult to 1.0 since prob value is not needed.
      float sampled_prob = need_prob_values ? p_sampled_probs[i] : 1.0;
      std::vector<TokenProbPair> top_prob_tokens;
      top_prob_tokens.reserve(top_prob_offset_indptr[i + 1] - top_prob_offset_indptr[i]);
      for (int j = top_prob_offset_indptr[i]; j < top_prob_offset_indptr[i + 1]; ++j) {
        top_prob_tokens.emplace_back(p_top_prob_indices[j], p_top_prob_probs[j]);
      }
      sample_results.push_back(
          SampleResult{{p_sampled_token_ids[i], sampled_prob}, top_prob_tokens});
    }

    RECORD_EVENT(trace_recorder_, request_ids, "finish sampling");
    return sample_results;
  }

  std::vector<std::vector<SampleResult>> BatchVerifyDraftTokens(
      NDArray probs_on_device, const Array<String>& request_ids,
      const std::vector<int>& cum_verify_lengths, const Array<GenerationConfig>& generation_cfg,
      const std::vector<RandomGenerator*>& rngs,
      const std::vector<std::vector<SampleResult>>& draft_output_tokens,
      const std::vector<std::vector<NDArray>>& draft_output_prob_dist) final {
    LOG(FATAL) << "GPU sampler does not support batch verification for now.";
  }

 private:
  /*! \brief Generate uniform random numbers, and copy the numbers and sample indices to GPU. */
  std::pair<NDArray, NDArray> CopySamplesAndIndicesToGPU(const std::vector<int>& sample_indices,
                                                         const std::vector<RandomGenerator*>& rngs,
                                                         int num_samples) {
    // Generate random numbers.
    float* p_uniform_samples = static_cast<float*>(uniform_samples_host_->data);
    int* p_sample_indices = static_cast<int*>(sample_indices_host_->data);
    for (int i = 0; i < num_samples; ++i) {
      p_uniform_samples[i] = rngs[i]->GetRandomNumber();
      p_sample_indices[i] = sample_indices[i];
    }
    // Copy the random numbers and sample indices to GPU.
    NDArray uniform_samples_host = uniform_samples_host_.CreateView({num_samples}, dtype_f32_);
    NDArray uniform_samples_device = uniform_samples_device_.CreateView({num_samples}, dtype_f32_);
    NDArray sample_indices_host = sample_indices_host_.CreateView({num_samples}, dtype_i32_);
    NDArray sample_indices_device = sample_indices_device_.CreateView({num_samples}, dtype_i32_);
    CopyArray(/*src=*/uniform_samples_host, /*dst=*/uniform_samples_device, copy_stream_);
    CopyArray(/*src=*/sample_indices_host, /*dst=*/sample_indices_device, copy_stream_);
    return {uniform_samples_device, sample_indices_device};
  }

  /*! \brief Check if top p and prob values are needed, and collect info when necessary. */
  void CheckTopPAndProbValues(const Array<GenerationConfig>& generation_cfg,
                              const std::vector<int>& sample_indices, int num_probs,
                              int num_samples, int vocab_size, bool* need_top_p,
                              bool* need_prob_values, std::vector<int>* top_prob_offset_indptr) {
    top_prob_offset_indptr->reserve(num_samples + 1);
    top_prob_offset_indptr->push_back(0);
    // Initialize top p values with -1.
    float* p_top_p = static_cast<float*>(top_p_host_->data);
    for (int i = 0; i < num_probs; ++i) {
      p_top_p[i] = -1.0;
    }
    int* p_top_prob_offsets = static_cast<int*>(top_prob_offsets_host_->data);
    int num_top_probs = 0;
    for (int i = 0; i < num_samples; ++i) {
      if (p_top_p[sample_indices[i]] == -1.0) {
        p_top_p[sample_indices[i]] = generation_cfg[i]->top_p;
        *need_top_p |= generation_cfg[i]->top_p != 1.0;
      } else {
        CHECK(fabs(p_top_p[sample_indices[i]] - generation_cfg[i]->top_p) < eps_)
            << "GPU sampler requires the top_p values for each prob distribution are the same.";
      }

      *need_prob_values |= generation_cfg[i]->logprobs;
      for (int j = 0; j < generation_cfg[i]->top_logprobs; ++j) {
        p_top_prob_offsets[num_top_probs++] = sample_indices[i] * vocab_size + j;
      }
      top_prob_offset_indptr->push_back(top_prob_offset_indptr->back() +
                                        generation_cfg[i]->top_logprobs);
    }
    ICHECK_EQ(num_top_probs, top_prob_offset_indptr->back());
  }

  /*! \brief Sample tokens on GPU. Take out the probability values when needed. */
  std::vector<NDArray> SampleOnGPU(NDArray probs_on_device, NDArray uniform_samples_device,
                                   NDArray sample_indices_device,  //
                                   bool need_top_p, bool need_prob_values, int num_probs,
                                   const std::vector<int>& top_prob_offset_indptr) {
    NDArray sampled_token_ids_device{nullptr};
    NDArray sampled_probs_device{nullptr};
    NDArray top_prob_probs_device{nullptr};
    NDArray top_prob_indices_device{nullptr};

    if (!need_top_p && !need_prob_values) {
      // - Short path: If top_p and prob values are not needed, we directly sample from multinomial.
      SyncCopyStream(device_, compute_stream_, copy_stream_);
      sampled_token_ids_device = gpu_multinomial_from_uniform_func_(
          probs_on_device, uniform_samples_device, sample_indices_device);
      return {sampled_token_ids_device, sampled_probs_device, top_prob_probs_device,
              top_prob_indices_device};
    }

    // - Argsort the probability.
    Array<NDArray> argsort_results = gpu_argsort_probs_func_(probs_on_device);
    ICHECK_EQ(argsort_results.size(), 2);
    NDArray sorted_probs_on_device = argsort_results[0];
    NDArray sorted_indices_on_device = argsort_results[1];

    // - Copy auxiliary array for top-p and prob values in ahead.
    NDArray top_p_device;
    NDArray top_prob_offsets_device;
    if (need_top_p) {
      NDArray top_p_host = top_p_host_.CreateView({num_probs}, dtype_f32_);
      top_p_device = top_p_device_.CreateView({num_probs}, dtype_f32_);
      CopyArray(/*src=*/top_p_host, /*dst=*/top_p_device, copy_stream_);
    }
    if (need_prob_values) {
      int num_top_probs = top_prob_offset_indptr.back();
      NDArray top_prob_offsets_host =
          top_prob_offsets_host_.CreateView({num_top_probs}, dtype_i32_);
      top_prob_offsets_device = top_prob_offsets_device_.CreateView({num_top_probs}, dtype_i32_);
      CopyArray(/*src=*/top_prob_offsets_host, /*dst=*/top_prob_offsets_device, copy_stream_);
    }
    SyncCopyStream(device_, compute_stream_, copy_stream_);

    if (need_top_p) {
      // - Sample with top_p applied.
      sampled_token_ids_device =
          gpu_sample_with_top_p_func_(sorted_probs_on_device, sorted_indices_on_device,
                                      uniform_samples_device, sample_indices_device, top_p_device);
    } else {
      // - Sample without top_p.
      sampled_token_ids_device = gpu_multinomial_from_uniform_func_(
          probs_on_device, uniform_samples_device, sample_indices_device);
    }

    if (need_prob_values) {
      // - Take the probability values.
      Array<NDArray> prob_value_results = gpu_sampler_take_probs_func_(
          probs_on_device, sorted_indices_on_device, sample_indices_device,
          sampled_token_ids_device, top_prob_offsets_device);
      sampled_probs_device = prob_value_results[0];
      top_prob_probs_device = prob_value_results[1];
      top_prob_indices_device = prob_value_results[2];
    }

    return {sampled_token_ids_device, sampled_probs_device, top_prob_probs_device,
            top_prob_indices_device};
  }

  /*! \brief Copy the results of GPU sampling functions back to CPU. */
  std::vector<NDArray> CopyArraysToCPU(const std::vector<NDArray>& device_arrays,  //
                                       int num_samples, bool need_prob_values, int num_top_probs) {
    NDArray sampled_token_ids_device = device_arrays[0];
    NDArray sampled_probs_device = device_arrays[1];
    NDArray top_prob_probs_device = device_arrays[2];
    NDArray top_prob_indices_device = device_arrays[3];
    ICHECK(sampled_token_ids_device.defined());
    ICHECK_EQ(sampled_token_ids_device->ndim, 1);
    ICHECK_EQ(sampled_token_ids_device->shape[0], num_samples);
    NDArray sampled_token_ids_host = sampled_token_ids_host_.CreateView({num_samples}, dtype_i32_);
    CopyArray(/*src=*/sampled_token_ids_device, /*dst=*/sampled_token_ids_host, compute_stream_);

    NDArray sampled_probs_host{nullptr};
    NDArray top_prob_probs_host{nullptr};
    NDArray top_prob_indices_host{nullptr};
    if (need_prob_values) {
      ICHECK(sampled_probs_device.defined());
      ICHECK(top_prob_probs_device.defined());
      ICHECK(top_prob_indices_device.defined());
      ICHECK_EQ(sampled_probs_device->ndim, 1);
      ICHECK_EQ(top_prob_probs_device->ndim, 1);
      ICHECK_EQ(top_prob_indices_device->ndim, 1);
      ICHECK_EQ(sampled_probs_device->shape[0], num_samples);
      ICHECK_EQ(top_prob_probs_device->shape[0], num_top_probs);
      ICHECK_EQ(top_prob_indices_device->shape[0], num_top_probs);
      sampled_probs_host = sampled_probs_host_.CreateView({num_samples}, dtype_i32_);
      top_prob_probs_host = top_prob_probs_host_.CreateView({num_top_probs}, dtype_f32_);
      top_prob_indices_host = top_prob_indices_host_.CreateView({num_top_probs}, dtype_i32_);
      CopyArray(/*src=*/sampled_probs_device, /*dst=*/sampled_probs_host, compute_stream_);
      if (num_top_probs > 0) {
        CopyArray(/*src=*/top_prob_probs_device, /*dst=*/top_prob_probs_host, compute_stream_);
        CopyArray(/*src=*/top_prob_indices_device, /*dst=*/top_prob_indices_host, compute_stream_);
      }
    }

    // Synchronize for CPU to get the correct array results.
    TVMSynchronize(device_.device_type, device_.device_id, nullptr);

    return {sampled_token_ids_host, sampled_probs_host, top_prob_probs_host, top_prob_indices_host};
  }

  // Model configurations
  const int max_num_sample_;
  const int vocab_size_;
  const DLDataType dtype_i32_ = DataType::Int(32);
  const DLDataType dtype_f32_ = DataType::Float(32);
  // Functions for sampling on GPU.
  Device device_;
  PackedFunc gpu_multinomial_from_uniform_func_;
  PackedFunc gpu_argsort_probs_func_;
  PackedFunc gpu_sample_with_top_p_func_;
  PackedFunc gpu_sampler_take_probs_func_;
  // Auxiliary NDArrays on CPU
  NDArray uniform_samples_host_;
  NDArray sample_indices_host_;
  NDArray top_p_host_;
  NDArray top_prob_offsets_host_;
  NDArray sampled_token_ids_host_;
  NDArray sampled_probs_host_;
  NDArray top_prob_probs_host_;
  NDArray top_prob_indices_host_;
  // Auxiliary NDArrays on GPU
  NDArray uniform_samples_device_;
  NDArray sample_indices_device_;
  NDArray top_p_device_;
  NDArray top_prob_offsets_device_;
  // The event trace recorder for requests. */
  Optional<EventTraceRecorder> trace_recorder_;
  // The device stream for the default computation operations.
  TVMStreamHandle compute_stream_ = nullptr;
  // The device stream for copying auxiliary data structure to GPU.
  TVMStreamHandle copy_stream_ = nullptr;
  const float eps_ = 1e-5;
};

Sampler Sampler::CreateGPUSampler(int max_num_sample, int vocab_size, FunctionTable* ft,
                                  DLDevice device, Optional<EventTraceRecorder> trace_recorder) {
  return Sampler(
      make_object<GPUSampler>(max_num_sample, vocab_size, ft, device, std::move(trace_recorder)));
}

}  // namespace serve
}  // namespace llm
}  // namespace mlc
