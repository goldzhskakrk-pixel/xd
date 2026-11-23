package com.example.voicechanger

object NativeBridge {
    init { System.loadLibrary("voicechanger") }
    external fun start(): Boolean
    external fun stop()
    external fun setParam(id: Int, value: Float)
    external fun level(which: Int): Float  // 0 = mikrofon girisi, 1 = islenmis cikis (dB)
}
