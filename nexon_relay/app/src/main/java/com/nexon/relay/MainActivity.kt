package com.nexon.relay

import android.annotation.SuppressLint
import android.os.Bundle
import android.view.ViewGroup
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.webkit.WebViewCompat
import androidx.webkit.WebViewFeature
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {
    private lateinit var webView: WebView
    private lateinit var statusBar: TextView
    private lateinit var relay: NexonRelay
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var relayScript: String = ""
    private var sitesJson: String = ""

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.databaseEnabled = true
            webViewClient = object : WebViewClient() {
                override fun onPageFinished(view: WebView, url: String) {
                    // Fallback path only. DOCUMENT_START below covers modern WebViews.
                    if (relayScript.isNotEmpty() &&
                        !WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
                        view.evaluateJavascript(relayScript, null)
                    }
                }
            }
        }

        relay = NexonRelay(webView, scope, ::logLine)
        relay.inputBackend = DispatchKeyEventBackend(
            composerSelectorProvider = { currentComposerSelector() },
            log = ::logLine
        )
        webView.addJavascriptInterface(relay, "NexonRelay")

        statusBar = TextView(this).apply {
            setPadding(32, 16, 32, 16)
            text = "nexon_relay: checking relay..."
            textSize = 12f
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(statusBar, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(webView, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(root)

        relayScript = assets.open("relay.js").bufferedReader().use { it.readText() }
        sitesJson = assets.open("sites.json").bufferedReader().use { it.readText() }
        installRelayScript()

        scope.launch { refreshHealth() }
        webView.loadUrl("https://chatgpt.com/")
    }

    private fun installRelayScript() {
        // Prepend the sites config so relay.js sees window.__nexonSites before it
        // ever scans. Both go in one document-start script so ordering is certain.
        val bootstrap = "window.__nexonSites = (" + sitesJson + ").sites;"
        val fullScript = bootstrap + "\n" + relayScript
        if (WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
            WebViewCompat.addDocumentStartJavaScript(webView, fullScript, setOf("*"))
            logLine("inject: document-start")
        } else {
            logLine("inject: fallback (onPageFinished)")
        }
    }

    /** First composer selector for the current host, joined for querySelector fallback. */
    private fun currentComposerSelector(): String {
        val fallback = "div[contenteditable='true'], textarea"
        val host = try { android.net.Uri.parse(webView.url ?: "").host } catch (e: Exception) { null }
        if (host.isNullOrEmpty()) return fallback
        return try {
            val sites = org.json.JSONObject(sitesJson).getJSONArray("sites")
            for (i in 0 until sites.length()) {
                val s = sites.getJSONObject(i)
                val hosts = s.getJSONArray("hosts")
                var match = false
                for (j in 0 until hosts.length()) if (hosts.getString(j) == host) match = true
                if (!match) continue
                val cs = s.getJSONArray("composerSelectors")
                val parts = (0 until cs.length()).map { cs.getString(it) }
                return parts.joinToString(", ")
            }
            fallback
        } catch (e: Exception) { fallback }
    }

    private suspend fun refreshHealth() {
        val h = relay.health()
        statusBar.text = if (h.contains("\"ok\":true")) {
            "nexon_relay: connected"
        } else {
            "nexon_relay: OFFLINE - start nexon_code1 in Termux"
        }
    }

    private fun logLine(msg: String) {
        android.util.Log.i("nexon_relay", msg)
    }

    override fun onDestroy() {
        scope.cancel()
        webView.destroy()
        super.onDestroy()
    }

    @Deprecated("Deprecated in API 33 but still the supported callback below that")
    override fun onBackPressed() {
        if (webView.canGoBack()) webView.goBack() else super.onBackPressed()
    }
}
