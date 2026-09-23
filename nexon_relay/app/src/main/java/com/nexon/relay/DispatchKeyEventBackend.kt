package com.nexon.relay

import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.webkit.WebView

/**
 * Types via WebView.dispatchKeyEvent - the host-app input path Chromium
 * marks as trusted. ASCII printable characters go through as real
 * keydown/keyup pairs. Characters the KeyEvent API cannot represent
 * (non-ASCII, emoji) fall back to a JS insertion - detectable, and logged
 * as such. v0.2's IME backend removes that fallback entirely.
 */
class DispatchKeyEventBackend(
    private val composerSelectorProvider: () -> String,
    private val log: (String) -> Unit
) : InputBackend {

    private val handler = Handler(Looper.getMainLooper())

    override fun typeText(webView: WebView, text: String) {
        val selector = composerSelectorProvider()
        // Focus the composer first so execCommand fallback lands somewhere.
        webView.evaluateJavascript(
            "(function(){var el=document.querySelector('$selector'); if(el) el.focus(); return !!el;})()",
            null
        )
        handler.postDelayed({ typeInto(webView, selector, text) }, 60)
    }

    override fun send(webView: WebView) {
        handler.postDelayed({
            val down = KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER)
            val up = KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER)
            webView.dispatchKeyEvent(down)
            webView.dispatchKeyEvent(up)
            log("relay: sent (enter)")
        }, 120)
    }

    private fun typeInto(webView: WebView, selector: String, text: String) {
        val sb = StringBuilder()
        var fallbackFrom = -1

        fun flush(until: Int) {
            if (fallbackFrom < 0) return
            val chunk = text.substring(fallbackFrom, until)
            val esc = jsString(chunk)
            webView.evaluateJavascript(
                "(function(){var el=document.querySelector('$selector'); if(!el) return; el.focus(); document.execCommand('insertText', false, $esc);})()",
                null
            )
            fallbackFrom = -1
        }

        var keyEventChars = 0
        var fallbackChars = 0
        for (i in text.indices) {
            val c = text[i]
            val kc = keyCodeFor(c)
            if (kc != null) {
                flush(i)
                webView.dispatchKeyEvent(KeyEvent(0, 0, KeyEvent.ACTION_DOWN, kc.first, 0, kc.second))
                webView.dispatchKeyEvent(KeyEvent(0, 0, KeyEvent.ACTION_UP, kc.first, 0, kc.second))
                keyEventChars++
            } else {
                if (fallbackFrom < 0) fallbackFrom = i
                fallbackChars++
            }
        }
        flush(text.length)
        log("relay: typed $keyEventChars keys, $fallbackChars via JS fallback")
    }

    private fun keyCodeFor(c: Char): Pair<Int, Int>? = when {
        c in 'a'..'z' -> KeyEvent.KEYCODE_A + (c - 'a') to 0
        c in 'A'..'Z' -> KeyEvent.KEYCODE_A + (c - 'A') to KeyEvent.META_SHIFT_ON
        c in '0'..'9' -> KeyEvent.KEYCODE_0 + (c - '0') to 0
        c == ' ' -> KeyEvent.KEYCODE_SPACE to 0
        c == '\n' -> KeyEvent.KEYCODE_ENTER to 0
        c == '\t' -> KeyEvent.KEYCODE_TAB to 0
        c == '.' -> KeyEvent.KEYCODE_PERIOD to 0
        c == ',' -> KeyEvent.KEYCODE_COMMA to 0
        c == ';' -> KeyEvent.KEYCODE_SEMICOLON to 0
        c == '\'' -> KeyEvent.KEYCODE_APOSTROPHE to 0
        c == '/' -> KeyEvent.KEYCODE_SLASH to 0
        c == '\\' -> KeyEvent.KEYCODE_BACKSLASH to 0
        c == '-' -> KeyEvent.KEYCODE_MINUS to 0
        c == '=' -> KeyEvent.KEYCODE_EQUALS to 0
        c == '[' -> KeyEvent.KEYCODE_LEFT_BRACKET to 0
        c == ']' -> KeyEvent.KEYCODE_RIGHT_BRACKET to 0
        c == '`' -> KeyEvent.KEYCODE_GRAVE to 0
        c == '!' -> KeyEvent.KEYCODE_1 to KeyEvent.META_SHIFT_ON
        c == '@' -> KeyEvent.KEYCODE_2 to KeyEvent.META_SHIFT_ON
        c == '#' -> KeyEvent.KEYCODE_3 to KeyEvent.META_SHIFT_ON
        c == '$' -> KeyEvent.KEYCODE_4 to KeyEvent.META_SHIFT_ON
        c == '%' -> KeyEvent.KEYCODE_5 to KeyEvent.META_SHIFT_ON
        c == '^' -> KeyEvent.KEYCODE_6 to KeyEvent.META_SHIFT_ON
        c == '&' -> KeyEvent.KEYCODE_7 to KeyEvent.META_SHIFT_ON
        c == '*' -> KeyEvent.KEYCODE_8 to KeyEvent.META_SHIFT_ON
        c == '(' -> KeyEvent.KEYCODE_9 to KeyEvent.META_SHIFT_ON
        c == ')' -> KeyEvent.KEYCODE_0 to KeyEvent.META_SHIFT_ON
        c == '_' -> KeyEvent.KEYCODE_MINUS to KeyEvent.META_SHIFT_ON
        c == '+' -> KeyEvent.KEYCODE_EQUALS to KeyEvent.META_SHIFT_ON
        c == '{' -> KeyEvent.KEYCODE_LEFT_BRACKET to KeyEvent.META_SHIFT_ON
        c == '}' -> KeyEvent.KEYCODE_RIGHT_BRACKET to KeyEvent.META_SHIFT_ON
        c == ':' -> KeyEvent.KEYCODE_SEMICOLON to KeyEvent.META_SHIFT_ON
        c == '"' -> KeyEvent.KEYCODE_APOSTROPHE to KeyEvent.META_SHIFT_ON
        c == '<' -> KeyEvent.KEYCODE_COMMA to KeyEvent.META_SHIFT_ON
        c == '>' -> KeyEvent.KEYCODE_PERIOD to KeyEvent.META_SHIFT_ON
        c == '?' -> KeyEvent.KEYCODE_SLASH to KeyEvent.META_SHIFT_ON
        c == '|' -> KeyEvent.KEYCODE_BACKSLASH to KeyEvent.META_SHIFT_ON
        c == '~' -> KeyEvent.KEYCODE_GRAVE to KeyEvent.META_SHIFT_ON
        else -> null
    }

    private fun jsString(s: String): String {
        val sb = StringBuilder("\"")
        for (c in s) {
            when (c) {
                '\\' -> sb.append("\\\\")
                '"' -> sb.append("\\\"")
                '\n' -> sb.append("\\n")
                '\r' -> sb.append("\\r")
                '\t' -> sb.append("\\t")
                else -> if (c.code < 0x20) sb.append("\\u%04x".format(c.code)) else sb.append(c)
            }
        }
        sb.append("\"")
        return sb.toString()
    }
}
