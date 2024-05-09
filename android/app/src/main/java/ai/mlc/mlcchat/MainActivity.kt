package ai.mlc.mlcchat

import ai.mlc.mlcllm.JSONFFIEngine
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.compose.material3.ExperimentalMaterial3Api
import java.io.File

import ai.mlc.mlcchat.ui.theme.MLCChatTheme
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier



class MainActivity : ComponentActivity() {

    @ExperimentalMaterial3Api
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
//        setContent {
//            Surface(
//                modifier = Modifier
//                    .fillMaxSize()
//            ) {
//                MLCChatTheme {
//                    NavView()
//                }
//            }
//        }
        val engine = MLCEngine()
        var modelPath = "phi-2-q4f16_1"
        modelPath = File(application.getExternalFilesDir(""), modelPath).toString()
        Log.i("MLC", "model path: $modelPath")
        val modelLib = "system://phi_msft_q4f16_1"
        val engineConfigJSONStr = """
            {
                "model": "$modelPath",
                "model_lib": "$modelLib",
                "mode": "interactive"
            }
        """.trimIndent()
        engine.reload(engineConfigJSONStr)
        Log.i("MLC", "engine loaded")
        val jsonRequest = """
            {
                "model": "phi-2-q4f16_1",
                "messages": [
                    {
                        "role": "user",
                        "content": [
                            { "type": "text", "text": "What is the meaning of life?" }
                        ]
                    }
                ]
            }
        """.trimIndent()
        val response = engine.chatCompletion(jsonRequest)
        Log.i("MLC", "response: $response")
    }
}