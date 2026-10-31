// Copyright (C) Honey Bunny QT
// SPDX-License-Identifier: AGPL-3.0-only

package dev.n3n.android

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.net.VpnService
import android.os.Build
import android.os.Bundle
import android.text.Editable
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.TextWatcher
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/**
 * The network at a glance: connect and disconnect, the state of the
 * connection as N3nVpnService has it from the edge, and the way to the
 * other screens.
 */
class MainActivity : Activity() {

    companion object {
        private const val REQUEST_VPN = 1
        // from the tile in the quick settings: connect as Connect does,
        // see ConnectTile
        const val EXTRA_CONNECT = "connect"
    }

    private lateinit var connect: ConnectButton
    private lateinit var network: TextView
    private lateinit var address: TextView
    private lateinit var supernode: TextView
    private lateinit var roundTrip: TextView
    private lateinit var peers: TextView
    private lateinit var hint: TextView
    private lateinit var reconnect: View
    private lateinit var routeAll: Switch
    private lateinit var gateway: EditText
    private lateinit var dns: EditText
    private lateinit var exits: LinearLayout
    // the exits shown, so that they are not made anew each second
    private var exitsShown: List<Status.Exit> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        connect = findViewById(R.id.connect)
        network = findViewById(R.id.network)
        address = findViewById(R.id.address)
        supernode = findViewById(R.id.supernode)
        roundTrip = findViewById(R.id.round_trip)
        peers = findViewById(R.id.peers)
        hint = findViewById(R.id.hint)
        reconnect = findViewById(R.id.reconnect)
        routeAll = findViewById(R.id.route_all)
        gateway = findViewById(R.id.gateway)
        dns = findViewById(R.id.dns)
        exits = findViewById(R.id.exits)
        setupRoute()

        connect.setOnClickListener { toggle() }
        findViewById<View>(R.id.scan).setOnClickListener {
            startActivity(Intent(this, EditActivity::class.java).putExtra(EditActivity.EXTRA_SCAN, true))
        }
        findViewById<View>(R.id.edit).setOnClickListener {
            startActivity(Intent(this, EditActivity::class.java))
        }
        findViewById<View>(R.id.share).setOnClickListener {
            startActivity(Intent(this, ShareActivity::class.java))
        }
        findViewById<View>(R.id.log).setOnClickListener {
            startActivity(Intent(this, LogActivity::class.java))
        }

        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(arrayOf("android.permission.POST_NOTIFICATIONS"), 0)
        }
        if (savedInstanceState == null) {
            connectIfAsked(intent)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        connectIfAsked(intent)
    }

    private fun connectIfAsked(intent: Intent?) {
        if (intent?.getBooleanExtra(EXTRA_CONNECT, false) == true && !N3nVpnService.running) {
            intent.removeExtra(EXTRA_CONNECT)
            toggle()
        }
    }

    override fun onResume() {
        super.onResume()
        showConfig()
        N3nVpnService.watch { show(it) }
    }

    override fun onPause() {
        N3nVpnService.watch(null)
        super.onPause()
    }

    /** The routing card: kept in the preferences as it is changed */
    private fun setupRoute() {
        val route = Store.route(this)
        routeAll.isChecked = route.all
        gateway.setText(route.gateway.ifEmpty { Config(Store.load(this)).gateway ?: "" })
        dns.setText(route.dns)
        findViewById<View>(R.id.route_row).visibility = if (route.all) View.VISIBLE else View.GONE

        val save = {
            Store.saveRoute(this, Store.Route(routeAll.isChecked, gateway.text.toString().trim(), dns.text.toString().trim()))
            // the exit peer's address may be what a tap needs
            show(N3nVpnService.state)
        }
        routeAll.setOnCheckedChangeListener { _, on ->
            findViewById<View>(R.id.route_row).visibility = if (on) View.VISIBLE else View.GONE
            save()
        }
        val watcher = object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable?) = save()
        }
        gateway.addTextChangedListener(watcher)
        dns.addTextChangedListener(watcher)
    }

    private fun showConfig() {
        val text = Store.load(this)
        val config = Config(text)
        val addr = config.address
        // the peer's own name; never the community's, which may be the key
        // of the headers
        network.text = when {
            text.isBlank() -> getString(R.string.no_network)
            else -> config.description ?: getString(R.string.unnamed)
        }
        address.text = when {
            addr != null -> "${addr.first}/${addr.second}"
            config.addressInvalid || text.isBlank() -> getString(R.string.none)
            else -> getString(R.string.address_auto)
        }
        findViewById<View>(R.id.share).isEnabled = text.isNotBlank()
        when {
            text.isBlank() -> showHint(R.string.no_config_hint)
            config.addressInvalid -> showHint(R.string.no_address)
            else -> hint.visibility = View.GONE
        }
    }

    private fun showHint(text: Int) {
        hint.setText(text)
        hint.visibility = View.VISIBLE
    }

    private fun toggle() {
        if (N3nVpnService.running) {
            N3nVpnService.stop(this)
            return
        }
        when (val why = N3nVpnService.notReady(this)) {
            null -> {}
            R.string.route_need_gateway -> {
                Toast.makeText(this, why, Toast.LENGTH_LONG).show()
                gateway.requestFocus()
                return
            }
            else -> {
                AlertDialog.Builder(this)
                    .setMessage(why)
                    .setPositiveButton(R.string.edit) { _, _ ->
                        startActivity(Intent(this, EditActivity::class.java))
                    }
                    .setNegativeButton(android.R.string.cancel, null)
                    .show()
                return
            }
        }
        // asks the user the first time; null when allowed already
        val ask = VpnService.prepare(this)
        if (ask != null) {
            startActivityForResult(ask, REQUEST_VPN)
        } else {
            start()
        }
    }

    private fun start() = N3nVpnService.start(this)

    @Deprecated("the platform's own Activity, no AndroidX")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_VPN && resultCode == RESULT_OK) {
            start()
        }
    }

    /** What the connection is doing, see Connection.kt */
    private fun show(s: Snapshot) {
        // in place of the line below the button: no jump when it comes or
        // goes
        val changed = N3nVpnService.changedSinceStart(this)
        reconnect.visibility = if (changed) View.VISIBLE else View.INVISIBLE
        val look = when {
            s.phase == Phase.CONNECTED -> ConnectButton.Look.ON
            s.phase.busy -> ConnectButton.Look.BUSY
            else -> ConnectButton.Look.OFF
        }
        // what it is doing, or else what a tap does
        val detail = s.doing(this) ?: getString(
            when {
                s.failed -> R.string.stopped_see_log
                s.phase == Phase.CONNECTED -> R.string.tap_disconnect
                else -> R.string.tap_connect
            }
        )
        // a tap while stopping connects again, once the edge is down
        val starts = s.phase == Phase.OFF || s.phase == Phase.STOPPING
        // what keeps a tap from connecting: the symbol is grey, and the
        // line below says what is missing (a tap says more)
        val missing = if (starts) N3nVpnService.notReady(this) else null
        val needs = when (missing) {
            null -> null
            R.string.no_address -> R.string.needs_address
            R.string.route_need_gateway -> R.string.needs_gateway
            else -> R.string.needs_config
        }
        connect.show(
            look, s.title(this),
            if (needs != null && s.phase == Phase.OFF && !s.failed) getString(needs) else detail,
            getString(if (starts) R.string.connect else R.string.disconnect),
            usable = missing == null)

        val info = if (s.phase == Phase.OFF) null else s.info
        val sn = info?.supernode
        val rtt = info?.rttMs
        supernode.text = if (sn.isNullOrEmpty()) getString(R.string.none) else sn
        // a row of its own: the address keeps its size while this changes
        roundTrip.text = if (!sn.isNullOrEmpty() && rtt != null) getString(R.string.rtt, rtt) else getString(R.string.none)
        peers.text = if (info != null) getString(R.string.peers_count, info.peers, info.direct) else getString(R.string.none)
        showExits(info?.exits ?: emptyList())
    }

    /** A dot in front of text: pink for an exit the edge's check confirmed,
     * blue for one that only says it is */
    private fun dotted(confirmed: Boolean, text: String): CharSequence {
        val s = SpannableStringBuilder("\u25CF  ").append(text)
        val colour = getColor(if (confirmed) R.color.accent else R.color.exit_says)
        s.setSpan(ForegroundColorSpan(colour), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        return s
    }

    /** The peers that announce themselves as exits, below the exit peer's
     * field: a tap puts one's address there, a long press says what its dot
     * means */
    private fun showExits(list: List<Status.Exit>) {
        if (list == exitsShown) {
            return
        }
        exitsShown = list
        // the label stays, the entries go
        exits.removeViews(1, exits.childCount - 1)
        for (e in list) {
            val entry = TextView(this)
            entry.text = dotted(e.confirmed, getString(R.string.exit_entry, e.name.ifEmpty { e.addr }, e.addr))
            entry.setTextColor(getColor(R.color.text))
            entry.textSize = 15f
            entry.setPadding(0, 12, 0, 12)
            entry.setOnClickListener {
                gateway.setText(e.addr)
                gateway.setSelection(e.addr.length)
            }
            val meaning = getString(if (e.confirmed) R.string.exit_confirmed else R.string.exit_says)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                entry.tooltipText = meaning
            } else {
                entry.setOnLongClickListener {
                    Toast.makeText(this, meaning, Toast.LENGTH_SHORT).show()
                    true
                }
            }
            exits.addView(entry)
        }
        exits.visibility = if (list.isEmpty()) View.GONE else View.VISIBLE
    }
}
