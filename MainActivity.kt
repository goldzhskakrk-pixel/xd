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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 2, pad, pad)
        }
        toggle = Button(this).apply { setOnClickListener { onToggle() } }
        root.addView(toggle)
        root.addView(TextView(this).apply {
            text = "Geri besleme (ıslık) olmaması için kulaklık tak."
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

    override fun onResume() { super.onResume(); refresh() }

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
