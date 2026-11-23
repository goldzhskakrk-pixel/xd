package com.example.voicechanger

object NativeBridge {
    init { System.loadLibrary("voicechanger") }
    external fun start(): Boolean
    external fun stop()
    external fun setParam(id: Int, value: Float)
}
