package com.nexon.relay

import android.webkit.WebView

/**
 * How the relay types text into the composer and submits.
 *
 * v0.1 ships DispatchKeyEventBackend, which dispatches real key events via
 * WebView.dispatchKeyEvent - Chromium treats these as trusted user input,
 * unlike JS-synthesised events.
 *
 * v0.2 will add ImeBackend, a companion InputMethodService that produces the
 * full compositionstart -> beforeinput -> input -> compositionend sequence
 * rich editors (ProseMirror, Lexical) need. Interface is deliberately tiny
 * so the swap is a one-line change in MainActivity.
 */
interface InputBackend {
    /** Type [text] into the composer of [webView]. Called on the main thread. */
    fun typeText(webView: WebView, text: String)

    /** Submit. Called on the main thread, ~100 ms after typeText. */
    fun send(webView: WebView)
}
