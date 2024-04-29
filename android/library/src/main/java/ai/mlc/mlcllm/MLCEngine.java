package ai.mlc.mlcllm;

import org.apache.tvm.Device;
import org.apache.tvm.Function;
import org.apache.tvm.Module;
import org.apache.tvm.TVMValue;

public class MLCEngine {
    private Module jsonFFIEngine;
    private Function initBackgroundEngineFunc;
    private Function reloadFunc;
    private Function unloadFunc;
    private Function resetFunc;
    private Function chatCompletionFunc;
    private Function abortFunc;
    private Function getLastErrorFunc;
    private Function runBackgroundLoopFunc;
    private Function runBackgroundStreamBackLoopFunc;
    private Function exitBackgroundLoopFunc;

    public MLCEngine() {
        Function createFunc = Function.getFunction("mlc.json_ffi.CreateJSONFFIEngine");
        assert createFunc != null;
        jsonFFIEngine = createFunc.invoke().asModule();
        initBackgroundEngineFunc = jsonFFIEngine.getFunction("init_background_engine");
        reloadFunc = jsonFFIEngine.getFunction("reload");
        unloadFunc = jsonFFIEngine.getFunction("unload");
        resetFunc = jsonFFIEngine.getFunction("reset");
        chatCompletionFunc = jsonFFIEngine.getFunction("chat_completion");
        abortFunc = jsonFFIEngine.getFunction("abort");
        getLastErrorFunc = jsonFFIEngine.getFunction("get_last_error");
        runBackgroundLoopFunc = jsonFFIEngine.getFunction("run_background_loop");
        runBackgroundStreamBackLoopFunc = jsonFFIEngine.getFunction("run_background_stream_back_loop");
        exitBackgroundLoopFunc = jsonFFIEngine.getFunction("exit_background_loop");
    }

    public void init(String jsonFFIEngineConfigStr, String engineConfigStr, KotlinFunction callback) {
        Device device = Device.opencl();

        Function requestStreamCallback = Function.convertFunc(new Function.Callback() {
            @Override
            public Object invoke(TVMValue... args) {
                final String chatCompletionStreamResponsesJSONStr = args[0].asString();
                callback.invoke(chatCompletionStreamResponsesJSONStr);
                return true;
            }
        });

        initBackgroundEngineFunc.pushArg(jsonFFIEngineConfigStr).pushArg(engineConfigStr).pushArg(device.deviceType).pushArg(device.deviceId)
                .pushArg(requestStreamCallback).invoke();
    }
    public void runBackgroundLoop() {
        runBackgroundLoopFunc.invoke();
    }
    public void runBackgroundStreamBackLoop() {
        runBackgroundStreamBackLoopFunc.invoke();
    }

    public void chatCompletion(String requestJSONStr, String requestId) {
        chatCompletionFunc.pushArg(requestJSONStr).pushArg(requestId).invoke();
    }
    public void exitBackgroundLoop() {
        exitBackgroundLoopFunc.invoke();
    }

    public void unload() {
        unloadFunc.invoke();
    }

    public interface KotlinFunction {
        void invoke(String arg);
    }

}
