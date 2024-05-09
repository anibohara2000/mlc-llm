package ai.mlc.mlcchat

import ai.mlc.mlcllm.JSONFFIEngine
import android.util.Log
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.launch
import kotlin.concurrent.thread

class MLCEngine {
    private val jsonFFIEngine = JSONFFIEngine()

    init {
        jsonFFIEngine.initBackgroundEngine()
        thread(start=true) {
            jsonFFIEngine.runBackgroundLoop()
        }
        thread(start=true) {
            jsonFFIEngine.runBackgroundStreamBackLoop()
        }
    }

    private fun streamCallback(text: String) {
        Log.i("MLCEngine", "streamCallback: $text")
    }

    private fun deinit() {
        jsonFFIEngine.exitBackgroundLoop()
    }

    fun reload(engineConfigJSONStr: String) {
        jsonFFIEngine.reload(engineConfigJSONStr)
    }

    private fun unload() {
        jsonFFIEngine.unload()
    }

    fun chatCompletion(requestJSONStr: String) {
        val requestId = "123"
        jsonFFIEngine.chatCompletion(requestJSONStr, requestId)
    }
}