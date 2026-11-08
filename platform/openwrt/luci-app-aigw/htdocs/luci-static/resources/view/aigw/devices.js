'use strict';
'require view';
'require rpc';
'require ui';
'require poll';

var callDevices = rpc.declare({ object: 'ag.device', method: 'list', expect: { devices: [] } });
var callRequest = rpc.declare({ object: 'ag.decision', method: 'request', params: [ 'action', 'target', 'ttl' ] });
var callZone = rpc.declare({ object: 'ag.device', method: 'zone', params: [ 'device', 'zone' ] });

/* text, never markup: host names and vendor strings come from the network */
function T(s) {
	return document.createTextNode(s == null ? '' : String(s));
}

function bytes(n) {
	return '%1024.1mB'.format(n || 0);
}

function ago(ts) {
	if (!ts) return '-';
	var s = Math.max(0, Math.floor(Date.now() / 1000) - ts);
	return s < 60 ? _('now') : _('%t ago').format(s);
}

function done(res, what) {
	if (res && res.ok)
		ui.addNotification(null, E('p', {}, T(what)), 'info');
	else
		ui.addNotification(null, E('p', {}, T((res && res.error) || _('Failed'))), 'danger');
}

return view.extend({
	load: function() {
		return callDevices();
	},

	act: function(action, d) {
		var who = d.mac || d.ip;

		return callRequest(action, who, 3600).then(function(res) {
			done(res, _('%s: %s for an hour (undo it under Alerts and decisions).').format(d.hostname || who, action));
		});
	},

	setZone: function(d) {
		var input = E('input', { 'class': 'cbi-input-text', 'value': d.zone || '', 'placeholder': 'iot' });

		ui.showModal(T(_('Zone of %s').format(d.hostname || d.mac || d.ip)), [
			E('p', {}, T(_('Policies can be limited to a zone, e.g. "iot" for cameras and plugs. Empty: no zone.'))),
			input,
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, T(_('Cancel'))), ' ',
				E('button', { 'class': 'btn cbi-button-action', 'click': function() {
					return callZone(d.mac || d.ip, input.value.trim()).then(function(res) {
						ui.hideModal();
						done(res, _('Zone saved.'));
					});
				} }, T(_('Save')))
			])
		]);
	},

	table: function(devs) {
		var self = this;
		var rows = devs.sort(function(a, b) { return (b.last_seen || 0) - (a.last_seen || 0); }).map(function(d) {
			var w = d.wifi || {};
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, [ E('strong', {}, T(d.hostname || '-')), E('br'), E('small', {}, T(d.type || '')) ]),
				E('td', { 'class': 'td' }, [ T(d.ip || '-'), E('br'), E('small', {}, T(d.mac || '')) ]),
				E('td', { 'class': 'td' }, T(d.zone || '-')),
				E('td', { 'class': 'td' }, T(w.connected ? _('%d dBm').format(w.signal) : '-')),
				E('td', { 'class': 'td' }, T(d.queries || 0)),
				E('td', { 'class': 'td' }, T(_('%s up, %s down').format(bytes(d.bytes_up), bytes(d.bytes_down)))),
				E('td', { 'class': 'td' }, T(ago(d.last_seen))),
				E('td', { 'class': 'td right' }, [
					E('button', { 'class': 'btn cbi-button-negative', 'title': _('Cut it off from everything but the gateway'),
						'click': ui.createHandlerFn(self, 'act', 'quarantine', d) }, T(_('Quarantine'))), ' ',
					E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'act', 'priority', d) }, T(_('Prioritize'))), ' ',
					E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'setZone', d) }, T(_('Zone')))
				])
			]);
		});

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, T(_('Device'))), E('th', { 'class': 'th' }, T(_('Address'))),
				E('th', { 'class': 'th' }, T(_('Zone'))), E('th', { 'class': 'th' }, T(_('Wi-Fi'))),
				E('th', { 'class': 'th' }, T(_('Lookups'))), E('th', { 'class': 'th' }, T(_('Traffic'))),
				E('th', { 'class': 'th' }, T(_('Last seen'))), E('th', { 'class': 'th' })
			])
		].concat(rows.length ? rows : [ E('tr', { 'class': 'tr placeholder' }, E('td', { 'class': 'td' }, T(_('No devices seen yet.')))) ]));
	},

	render: function(devs) {
		var box = E('div', {}, this.table(devs));
		var self = this;

		poll.add(function() {
			return callDevices().then(function(d) { box.replaceChildren(self.table(d)); });
		}, 15);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, T(_('Devices'))),
			E('div', { 'class': 'cbi-map-descr' }, T(_('Everything AIGW has seen on your network. Actions last an hour and can be undone.'))),
			E('div', { 'class': 'cbi-section' }, box)
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
