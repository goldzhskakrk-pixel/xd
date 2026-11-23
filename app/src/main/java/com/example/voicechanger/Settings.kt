package com.example.voicechanger

import android.content.Context

object Settings {
    const val PITCH = 0
    const val LOW_SHELF = 1
    const val PRESENCE = 2
    const val DEESS_THRESHOLD = 3
    const val COMP_THRESHOLD = 4
    const val GAIN = 5

    // Varsayılanlar KALİBRE EDİLMEMİŞTİR: senin sesinle kulakla ayarlanacak.
    data class Def(val id: Int, val label: String, val min: Float, val max: Float, val default: Float)

    val defs = listOf(
        Def(PITCH, "Perde (yarım ton)", -6f, 3f, -1f),
        Def(LOW_SHELF, "Kalınlık (dB)", 0f, 12f, 6f),
        Def(PRESENCE, "Netlik (dB)", 0f, 10f, 4f),
        Def(DEESS_THRESHOLD, "De-esser eşiği (dB)", -40f, -10f, -30f),
        Def(COMP_THRESHOLD, "Kompresör eşiği (dB)", -30f, -6f, -18f),
        Def(GAIN, "Ses seviyesi (dB)", 0f, 30f, 12f),
    )

    private fun prefs(c: Context) = c.getSharedPreferences("vc", Context.MODE_PRIVATE)
    fun get(c: Context, d: Def): Float = prefs(c).getFloat("p${d.id}", d.default)
    fun set(c: Context, d: Def, v: Float) {
        prefs(c).edit().putFloat("p${d.id}", v).apply()
        NativeBridge.setParam(d.id, v)
    }
    fun pushAll(c: Context) = defs.forEach { NativeBridge.setParam(it.id, get(c, it)) }
}
