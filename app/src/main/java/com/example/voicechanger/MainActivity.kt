package com.example.voicechanger

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView

class MainActivity : Activity() {
    private lateinit var toggle: Button
    private lateinit var meter: TextView
    private val tick = object : Runnable {
        override fun run() {
            meter.text = if (VoiceService.running)
                "Mikrofon: %.0f dB   İşlenmiş çıkış: %.0f dB".format(NativeBridge.level(0), NativeBridge.level(1))
            else "Mikrofon: kapalı"
            meter.postDelayed(this, 200)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 2, pad, pad)
        }
        toggle = Button(this).apply { setOnClickListener { onToggle() } }
        root.addView(toggle)
        meter = TextView(this)
        root.addView(meter)
        root.addView(LinearLayout(this).apply {
            fun preset(name: String, values: Map<Int, Float>) = Button(this@MainActivity).apply {
                text = name
                setOnClickListener { Settings.applyPreset(this@MainActivity, values); recreate() }
            }
            addView(preset("Genç (18 yaş)", Settings.presetYoung))
            addView(preset("Spiker", Settings.presetAnnouncer))
        })
        root.addView(TextView(this).apply {
            text = "Kendi sesini duymamak için \"Kendi sesimi dinleme\" 0 kalsın. Dinlemek istersen kulaklık tak (hoparlörde ıslık yapar)."
            setPadding(0, pad, 0, pad)
        })
        for (d in Settings.defs) {
            val label = TextView(this)
            val bar = SeekBar(this).apply { max = 1000 }
            fun show(v: Float) { label.text = "${d.label}: ${"%.1f".format(v)}" }
            val cur = Settings.get(this, d)
            bar.progress = (((cur - d.min) / (d.max - d.min)) * 1000f).toInt().coerceIn(0, 1000)
            show(cur)
            bar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) {
                    val v = d.min + (d.max - d.min) * p / 1000f
                    show(v)
                    if (fromUser) Settings.set(this@MainActivity, d, v)
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            })
            root.addView(label)
            root.addView(bar)
        }
        setContentView(ScrollView(this).apply { addView(root) })
    }

    override fun onResume() { super.onResume(); refresh(); meter.post(tick) }
    override fun onPause() { meter.removeCallbacks(tick); super.onPause() }

    private fun refresh() {
        toggle.text = if (VoiceService.running) "KAPAT" else "AÇ"
    }

    private fun missingPermissions(): Array<String> {
        val need = mutableListOf<String>()
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            need += Manifest.permission.RECORD_AUDIO
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            need += Manifest.permission.POST_NOTIFICATIONS
        return need.toTypedArray()
    }

    private fun onToggle() {
        if (VoiceService.running) {
            startService(Intent(this, VoiceService::class.java).setAction(VoiceService.ACTION_STOP))
            VoiceService.running = false
            refresh()
            return
        }
        val need = missingPermissions()
        if (need.isNotEmpty()) { requestPermissions(need, 1); return }
        startForegroundService(Intent(this, VoiceService::class.java))
        toggle.postDelayed({ refresh() }, 400)
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, res: IntArray) {
        super.onRequestPermissionsResult(code, perms, res)
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) onToggle()
    }
}
