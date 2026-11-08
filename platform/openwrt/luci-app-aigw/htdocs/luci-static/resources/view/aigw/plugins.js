'use strict';
'require view';
'require rpc';
'require ui';

var callPlugins = rpc.declare({ object: 'ag.plugin', method: 'list', expect: { plugins: [] } });
var callSet = rpc.declare({ object: 'ag.plugin', method: 'set', params: [ 'name', 'allow', 'enabled' ] });
var callModels = rpc.declare({ object: 'ag.model', method: 'list', expect: { '': {} } });
var callRollback = rpc.declare({ object: 'ag.model', method: 'rollback', params: [ 'name' ] });

function failed(res) {
	if (res && res.ok)
		return false;
	ui.addNotification(null, E('p', (res && res.error) || _('Failed')), 'danger');
	return true;
}

return view.extend({
	load: function() {
		return Promise.all([ callPlugins(), L.resolveDefault(callModels(), {}) ]);
	},

	reload: function() {
		var self = this;
		return this.load().then(function(d) { self.fill(d); });
	},

	set: function(name, what, on) {
		var self = this;
		return callSet(name, what == 'allow' ? on : null, what == 'enabled' ? on : null).then(function(res) {
			failed(res);
			return self.reload();
		});
	},

	rollback: function(name) {
		var self = this;
		if (!confirm(_('Go back to the previous %s and hold this version off?').format(name)))
			return;
		return callRollback(name).then(function(res) { failed(res); return self.reload(); });
	},

	plugins: function(list) {
		var self = this;
		var rows = list.map(function(p) {
			var btns = [], from = p.signed_by ? _('signed (%s)').format(p.signed_by) : p.source == 'package' ? _('package') : _('added by you');

			if (p.state == 'disabled')
				btns.push(E('button', { 'class': 'btn cbi-button-positive', 'click': ui.createHandlerFn(self, 'set', p.name, 'enabled', true) }, _('Turn on')));
			else
				btns.push(E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'set', p.name, 'enabled', false) }, _('Turn off')));
			if (p.source != 'package' && !p.signed_by) {
				btns.push(' ');
				btns.push(p.trusted
					? E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'set', p.name, 'allow', false) }, _('Deny'))
					: E('button', { 'class': 'btn cbi-button-action', 'title': (p.waiting_for_ok || []).join(', '),
						'click': ui.createHandlerFn(self, 'set', p.name, 'allow', true) }, _('Allow')));
			}
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, [ E('strong', p.name), ' ', E('small', p.version), E('br'), E('small', p.description) ]),
				E('td', { 'class': 'td' }, p.kind),
				E('td', { 'class': 'td' }, [ E('strong', p.state), p.why ? E('br') : '', p.why ? E('small', p.why) : '' ]),
				E('td', { 'class': 'td' }, [ from, E('br'), E('small', p.trusted ? _('trusted') : _('privileged capabilities need your OK')),
					p.signature ? E('br') : '', p.signature ? E('small', p.signature) : '' ]),
				E('td', { 'class': 'td right' }, btns)
			]);
		});

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [ E('th', { 'class': 'th' }, _('Plug-in')), E('th', { 'class': 'th' }, _('Kind')),
				E('th', { 'class': 'th' }, _('State')), E('th', { 'class': 'th' }, _('From')), E('th', { 'class': 'th' }) ])
		].concat(rows.length ? rows : [ E('tr', { 'class': 'tr placeholder' }, E('td', { 'class': 'td' }, _('No plug-ins.'))) ]));
	},

	models: function(m) {
		var self = this;
		var rows = (m.models || []).map(function(e) {
			var what = !e.loaded ? (e.kind == 'list' ? _('none yet (comes from cortex)') : _('not loaded'))
				: e.kind == 'list' ? _('version %d, %d names').format(e.version, e.entries)
				: e.source == 'package' ? _('packaged, %d trees').format(e.trees)
				: _('version %d, %d trees').format(e.version, e.trees);
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, E('strong', e.name)),
				E('td', { 'class': 'td' }, [ what, e.held_back ? E('br') : '', e.held_back ? E('small', _('version %d and older held back').format(e.held_back)) : '' ]),
				E('td', { 'class': 'td' }, e.signer ? _('%s, signed by %s').format(e.origin, e.signer) : (e.origin || '-')),
				E('td', { 'class': 'td' }, e.refused ? E('small', e.refused) : '-'),
				E('td', { 'class': 'td right' }, e.source == 'update'
					? E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'rollback', e.name) }, _('Roll back')) : '')
			]);
		});
		var keys = (m.trusted_keys || []).map(function(k) { return '%s (%s)'.format(k.fingerprint, k.comment); });

		return E('div', {}, [
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [ E('th', { 'class': 'th' }, _('Model')), E('th', { 'class': 'th' }, _('In use')),
					E('th', { 'class': 'th' }, _('From')), E('th', { 'class': 'th' }, _('Last refused')), E('th', { 'class': 'th' }) ])
			].concat(rows)),
			E('p', { 'class': 'cbi-section-descr' }, keys.length ? _('Trusted keys: %s').format(keys.join(', '))
				: _('No trusted keys: model updates are refused until you trust one (agctl trust <key.pub>).'))
		]);
	},

	fill: function(data) {
		this.pbox.replaceChildren(this.plugins(data[0]));
		this.mbox.replaceChildren(this.models(data[1]));
	},

	render: function(data) {
		this.pbox = E('div', {});
		this.mbox = E('div', {});
		this.fill(data);
		return E('div', { 'class': 'cbi-map' }, [
			E('h2', _('Plug-ins and models')),
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Plug-ins')),
				E('p', { 'class': 'cbi-section-descr' }, _('Plug-ins wire themselves in. Ones you added need your OK for privileged capabilities, unless they are signed by a key you trust.')),
				this.pbox ]),
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Models and lists')), this.mbox ])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
