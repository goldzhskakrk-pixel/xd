package com.example.voicechanger

import android.content.Context

object Settings {
    const val PITCH = 0
    const val LOW_SHELF = 1
    const val PRESENCE = 2
    const val DEESS_THRESHOLD = 3
    const val COMP_THRESHOLD = 4
    const val GAIN = 5
    const val GATE = 6
    const val FORMANT = 7
    const val MONITOR = 8

    // Varsayılanlar KALİBRE EDİLMEMİŞTİR: senin sesinle kulakla ayarlanacak.
    data class Def(val id: Int, val label: String, val min: Float, val max: Float, val default: Float)

    // Sira ve varsayilanlar app/src/main/cpp/dsp.h icindeki kDefaults ile ayni olmali.
    val defs = listOf(
        Def(PITCH, "Perde (yarım ton)", -8f, 4f, -1f),
        Def(LOW_SHELF, "Kalınlık (dB)", 0f, 10f, 1f),
        Def(PRESENCE, "Netlik (dB)", 0f, 8f, 5f),
        Def(DEESS_THRESHOLD, "De-esser eşiği (dB)", -40f, -10f, -30f),
        Def(COMP_THRESHOLD, "Kompresör eşiği (dB)", -30f, -6f, -22f),
        Def(GAIN, "Çıkış seviyesi (dB)", 0f, 24f, 6f),
        Def(GATE, "Gürültü kapısı (dB)", -70f, -30f, -55f),
        Def(FORMANT, "Ses tonu kalınlığı / formant (yarım ton)", -4f, 2f, 1f),
        Def(MONITOR, "Kendi sesimi dinleme (%)", 0f, 100f, 0f),
    )

    // Hazir sesler (KALIBRE EDILMEMISTIR, kulakla ayarlanacak).
    val presetYoung = mapOf(PITCH to -1f, LOW_SHELF to 1f, PRESENCE to 5f, COMP_THRESHOLD to -22f, FORMANT to 1f)
    val presetAnnouncer = mapOf(PITCH to -4f, LOW_SHELF to 6f, PRESENCE to 4f, COMP_THRESHOLD to -24f, FORMANT to -2f)
    fun applyPreset(c: Context, p: Map<Int, Float>) = p.forEach { (id, v) -> set(c, defs.first { it.id == id }, v) }

    private fun prefs(c: Context) = c.getSharedPreferences("vc3", Context.MODE_PRIVATE)
    fun get(c: Context, d: Def): Float = prefs(c).getFloat("p${d.id}", d.default)
    fun set(c: Context, d: Def, v: Float) {
        prefs(c).edit().putFloat("p${d.id}", v).apply()
        NativeBridge.setParam(d.id, v)
    }
    fun pushAll(c: Context) = defs.forEach { NativeBridge.setParam(it.id, get(c, it)) }
}
