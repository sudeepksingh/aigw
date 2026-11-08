'use strict';
'require view';
'require rpc';
'require ui';

var callKeys = rpc.declare({ object: 'ag.wifi', method: 'keys', expect: { '': {} } });
var callKeyAdd = rpc.declare({ object: 'ag.wifi', method: 'key_add', params: [ 'name', 'zone' ] });
var callKeyRevoke = rpc.declare({ object: 'ag.wifi', method: 'key_revoke', params: [ 'name' ] });
var callRemote = rpc.declare({ object: 'ag.remote', method: 'list', expect: { '': {} } });
var callRemoteOn = rpc.declare({ object: 'ag.remote', method: 'on', params: [ 'endpoint' ] });
var callRemoteOff = rpc.declare({ object: 'ag.remote', method: 'off' });
var callPeerAdd = rpc.declare({ object: 'ag.remote', method: 'add', params: [ 'name' ] });
var callPeerRevoke = rpc.declare({ object: 'ag.remote', method: 'revoke', params: [ 'name' ] });

/* text, never markup: names, SSIDs and endpoints are user or network input */
function T(s) {
	return document.createTextNode(s == null ? '' : String(s));
}

/* a QR code from agd's rows of '1'/'0', as SVG */
function qrSvg(rows) {
	var n = rows.length, q = 4, d = '', ns = 'http://www.w3.org/2000/svg';
	var svg = document.createElementNS(ns, 'svg'), path = document.createElementNS(ns, 'path');

	for (var y = 0; y < n; y++)
		for (var x = 0; x < n; x++)
			if (rows[y].charAt(x) == '1')
				d += 'M' + (x + q) + ' ' + (y + q) + 'h1v1h-1z';
	svg.setAttribute('viewBox', '0 0 ' + (n + 2 * q) + ' ' + (n + 2 * q));
	svg.setAttribute('width', '280');
	svg.setAttribute('height', '280');
	svg.setAttribute('shape-rendering', 'crispEdges');
	svg.setAttribute('style', 'background:#fff');
	path.setAttribute('d', d);
	path.setAttribute('fill', '#000');
	svg.appendChild(path);
	return svg;
}

function failed(res) {
	if (res && res.ok)
		return false;
	ui.addNotification(null, E('p', {}, T((res && res.error) || _('Failed'))), 'danger');
	return true;
}

function ago(ts) {
	if (!ts) return _('never yet');
	return _('%t ago').format(Math.max(0, Math.floor(Date.now() / 1000) - ts));
}

return view.extend({
	load: function() {
		return Promise.all([ L.resolveDefault(callKeys(), {}), L.resolveDefault(callRemote(), {}) ]);
	},

	reload: function() {
		var self = this;
		return this.load().then(function(d) { self.fill(d); });
	},

	showQr: function(title, rows, text, note) {
		ui.showModal(T(title), [
			E('div', { 'class': 'center' }, rows && rows.length ? qrSvg(rows) : E('em', {}, T(_('Too long for a QR code.')))),
			text ? E('pre', { 'style': 'white-space:pre-wrap;font-size:90%' }, T(text)) : '',
			E('p', {}, T(note)),
			E('div', { 'class': 'right' }, E('button', { 'class': 'btn', 'click': ui.hideModal }, T(_('Done'))))
		]);
	},

	addKey: function(name, zone) {
		var self = this;

		if (!name.value.trim())
			return;
		return callKeyAdd(name.value.trim(), zone.value.trim() || null).then(function(res) {
			if (failed(res)) return;
			name.value = '';
			self.showQr(_('Wi-Fi key for %s').format(res.name), res.qr_rows, _('Network %s, password %s').format(res.ssid, res.passphrase),
				_('Scan it with the device (or type the password). The first device that joins with it owns it.'));
			return self.reload();
		});
	},

	revokeKey: function(name) {
		var self = this;
		if (!confirm(_('Take the key away from %s and disconnect it?').format(name)))
			return;
		return callKeyRevoke(name).then(function(res) { failed(res); return self.reload(); });
	},

	remoteOn: function(endpoint) {
		var self = this;
		return callRemoteOn(endpoint.value.trim()).then(function(res) { failed(res); return self.reload(); });
	},

	remoteOff: function() {
		var self = this;
		if (!confirm(_('Turn remote access off? Connected phones lose their tunnel.')))
			return;
		return callRemoteOff().then(function(res) { failed(res); return self.reload(); });
	},

	addPeer: function(name) {
		var self = this;

		if (!name.value.trim())
			return;
		return callPeerAdd(name.value.trim()).then(function(res) {
			if (failed(res)) return;
			name.value = '';
			self.showQr(_('Remote access for %s').format(res.name), res.qr_rows, null,
				_('Scan it with the WireGuard app (+, scan from QR code). The private key is not kept: this is the only time it is shown.'));
			return self.reload();
		});
	},

	revokePeer: function(name) {
		var self = this;
		if (!confirm(_('Take remote access away from %s?').format(name)))
			return;
		return callPeerRevoke(name).then(function(res) { failed(res); return self.reload(); });
	},

	th: function(labels) {
		return E('tr', { 'class': 'tr table-titles' }, labels.map(function(l) { return E('th', { 'class': 'th' }, T(l)); }));
	},

	empty: function(text) {
		return E('tr', { 'class': 'tr placeholder' }, E('td', { 'class': 'td' }, T(text)));
	},

	keysSection: function(k) {
		var self = this, name = E('input', { 'class': 'cbi-input-text', 'placeholder': _('e.g. Living room plug') });
		var zone = E('input', { 'class': 'cbi-input-text', 'placeholder': _('zone, e.g. iot (optional)'), 'style': 'width:12em' });
		var rows = (k.keys || []).map(function(e) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, E('strong', {}, T(e.name))),
				E('td', { 'class': 'td' }, T(e.zone || '-')),
				E('td', { 'class': 'td' }, T(e.bound || e.mac || _('waiting for its device'))),
				E('td', { 'class': 'td right' }, [
					e.qr_rows ? E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'showQr',
						_('Wi-Fi key for %s').format(e.name), e.qr_rows, null, '') }, T(_('QR code'))) : '', ' ',
					E('button', { 'class': 'btn cbi-button-negative', 'click': ui.createHandlerFn(self, 'revokeKey', e.name) }, T(_('Revoke')))
				])
			]);
		});

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, T(_('Wi-Fi keys per device'))),
			E('p', { 'class': 'cbi-section-descr' }, T(_('Each device can have its own Wi-Fi password: revoke one without touching the others.'))),
			E('table', { 'class': 'table' }, [ this.th([ _('Name'), _('Zone'), _('Device'), '' ]) ]
				.concat(rows.length ? rows : [ this.empty(_('No per-device keys yet.')) ])),
			E('div', {}, [ name, ' ', zone, ' ',
				E('button', { 'class': 'btn cbi-button-add', 'click': ui.createHandlerFn(this, 'addKey', name, zone) }, T(_('New key'))) ])
		]);
	},

	remoteSection: function(r) {
		var self = this;

		if (!r.enabled) {
			var ep = E('input', { 'class': 'cbi-input-text', 'value': r.endpoint || '', 'placeholder': _('public address or DDNS name') });
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, T(_('Remote access'))),
				E('p', { 'class': 'cbi-section-descr' }, T(_('Reach home from your phone over WireGuard; its lookups go through AIGW wherever it is. Behind another router, forward UDP %d to this one.').format(r.port || 51820))),
				r.error ? E('p', { 'class': 'alert-message warning' }, T(r.error)) : '',
				E('div', {}, [ ep, ' ', E('button', { 'class': 'btn cbi-button-apply', 'click': ui.createHandlerFn(this, 'remoteOn', ep) }, T(_('Turn on'))) ])
			]);
		}
		var name = E('input', { 'class': 'cbi-input-text', 'placeholder': _("e.g. Anna's phone") });
		var rows = (r.peers || []).map(function(p) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, E('strong', {}, T(p.name))),
				E('td', { 'class': 'td' }, T(p.ip)),
				E('td', { 'class': 'td' }, T(p.online ? _('connected') : ago(p.handshake))),
				E('td', { 'class': 'td' }, T(p.endpoint || '-')),
				E('td', { 'class': 'td right' }, E('button', { 'class': 'btn cbi-button-negative', 'click': ui.createHandlerFn(self, 'revokePeer', p.name) }, T(_('Revoke'))))
			]);
		});

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, T(_('Remote access'))),
			E('p', { 'class': 'cbi-section-descr' }, T(_('On: %s, UDP %d, tunnel %s.').format(r.endpoint, r.port, r.subnet))),
			r.error ? E('p', { 'class': 'alert-message warning' }, T(r.error)) : '',
			E('table', { 'class': 'table' }, [ this.th([ _('Peer'), _('Address'), _('Last seen'), _('From'), '' ]) ]
				.concat(rows.length ? rows : [ this.empty(_('No peers yet.')) ])),
			E('div', {}, [ name, ' ',
				E('button', { 'class': 'btn cbi-button-add', 'click': ui.createHandlerFn(this, 'addPeer', name) }, T(_('Add a phone'))), ' ',
				E('button', { 'class': 'btn cbi-button-negative', 'click': ui.createHandlerFn(this, 'remoteOff') }, T(_('Turn off'))) ])
		]);
	},

	fill: function(data) {
		this.box.replaceChildren(this.keysSection(data[0]), this.remoteSection(data[1]));
	},

	render: function(data) {
		this.box = E('div', {});
		this.fill(data);
		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, T(_('Wi-Fi keys and remote access'))),
			this.box
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
