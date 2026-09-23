package com.nexon.relay

import android.webkit.JavascriptInterface
import android.webkit.WebView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL

/**
 * The one object exposed to page JavaScript as `window.NexonRelay`.
 *
 * Two methods, both non-blocking from JS's point of view:
 *   NexonRelay.run(cmdJson)         -> fire a command at the embedded
 *                                      relay server in nexon_code1
 *   NexonRelay.log(msg)             -> surface a line in the in-app log
 *
 * `run` returns via evaluateJavascript back into the page, because the
 * @JavascriptInterface call itself must not block the WebView's JS thread.
 */
class NexonRelay(
    private val webView: WebView,
    private val scope: CoroutineScope,
    private val log: (String) -> Unit,
    private val baseUrl: String = "http://127.0.0.1:8787"
) {
    /** Set by MainActivity after construction. Null disables typing. */
    var inputBackend: InputBackend? = null

    @JavascriptInterface
    fun run(cmdJson: String) {
        log("relay: -> " + cmdJson.take(120).replace("\n", " "))
        scope.launch {
            val result = postRun(cmdJson)
            withContext(Dispatchers.Main) {
                deliver(result)
            }
        }
    }

    @JavascriptInterface
    fun log(msg: String) {
        log("page: " + msg)
    }

    /**
     * Called by relay.js once a successful /run envelope has been parsed and
     * its stdout extracted. Types that stdout into the composer and hits
     * send. The backend decides *how* (key events in v0.1, IME in v0.2).
     */
    @JavascriptInterface
    fun typeAndSend(text: String) {
        if (text.isEmpty()) {
            log("relay: empty stdout, skipping type")
            return
        }
        log("relay: type+send (" + text.length + " chars)")
        val backend = inputBackend
        if (backend == null) {
            log("relay: no input backend installed")
            return
        }
        scope.launch(Dispatchers.Main) {
            backend.typeText(webView, text)
            backend.send(webView)
        }
    }

    /**
     * Hand the response back into the page as a JS callback. The injected
     * script defines window.__nexonRelayResult(jsonText) and decides what to
     * do with it (type it into the composer, or surface an error).
     */
    private fun deliver(resultJson: String) {
        val escaped = JSONObject.quote(resultJson)
        webView.evaluateJavascript(
            "window.__nexonRelayResult && window.__nexonRelayResult($escaped);",
            null
        )
    }

    private suspend fun postRun(cmdJson: String): String = withContext(Dispatchers.IO) {
        val body = JSONObject()
            .put("cmd", cmdJson)
            .put("timeout_ms", 120000)
            .toString()

        var conn: HttpURLConnection? = null
        try {
            val url = URL("$baseUrl/run")
            conn = (url.openConnection() as HttpURLConnection).apply {
                requestMethod = "POST"
                doOutput = true
                connectTimeout = 5000
                readTimeout = 130000
                setRequestProperty("Content-Type", "application/json")
            }
            conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }

            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.let { s ->
                BufferedReader(InputStreamReader(s, Charsets.UTF_8)).use { it.readText() }
            } ?: ""

            if (code !in 200..299) {
                return@withContext errorEnvelope("relay HTTP $code", text)
            }
            if (text.isBlank()) {
                return@withContext errorEnvelope("empty relay response", "")
            }
            return@withContext text
        } catch (e: Exception) {
            return@withContext errorEnvelope("relay unreachable: " + e.message, "")
        } finally {
            conn?.disconnect()
        }
    }

    private fun errorEnvelope(err: String, detail: String): String {
        val obj = JSONObject()
            .put("ok", false)
            .put("err", err)
            .put("exit", -1)
        if (detail.isNotBlank()) obj.put("detail", detail.take(400))
        return obj.toString()
    }

    /** GET /health, used by the UI to show relay status. */
    suspend fun health(): String = withContext(Dispatchers.IO) {
        var conn: HttpURLConnection? = null
        try {
            conn = (URL("$baseUrl/health").openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 2000
                readTimeout = 2000
            }
            val text = conn.inputStream.use { s ->
                BufferedReader(InputStreamReader(s, Charsets.UTF_8)).use { it.readText() }
            }
            return@withContext text
        } catch (e: Exception) {
            return@withContext "{\"ok\":false,\"err\":" + JSONObject.quote(e.message ?: "unreachable") + "}"
        } finally {
            conn?.disconnect()
        }
    }
}
