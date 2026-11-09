'use strict';
'require view';
'require rpc';
'require ui';

var callPlugins = rpc.declare({ object: 'ag.plugin', method: 'list', expect: { plugins: [] } });
var callSet = rpc.declare({ object: 'ag.plugin', method: 'set', params: [ 'name', 'allow', 'enabled' ] });
var callModels = rpc.declare({ object: 'ag.model', method: 'list', expect: { '': {} } });
var callRollback = rpc.declare({ object: 'ag.model', method: 'rollback', params: [ 'name' ] });

/* text, never markup: plug-in descriptions and refusal reasons are not ours */
function T(s) {
	return document.createTextNode(s == null ? '' : String(s));
}

function failed(res) {
	if (res && res.ok)
		return false;
	ui.addNotification(null, E('p', {}, T((res && res.error) || _('Failed'))), 'danger');
	return true;
}

function th(labels) {
	return E('tr', { 'class': 'tr table-titles' }, labels.map(function(l) { return E('th', { 'class': 'th' }, T(l)); }));
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
				btns.push(E('button', { 'class': 'btn cbi-button-positive', 'click': ui.createHandlerFn(self, 'set', p.name, 'enabled', true) }, T(_('Turn on'))));
			else
				btns.push(E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'set', p.name, 'enabled', false) }, T(_('Turn off'))));
			if (p.source != 'package' && !p.signed_by) {
				btns.push(' ');
				btns.push(p.trusted
					? E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'set', p.name, 'allow', false) }, T(_('Deny')))
					: E('button', { 'class': 'btn cbi-button-action', 'title': (p.waiting_for_ok || []).join(', '),
						'click': ui.createHandlerFn(self, 'set', p.name, 'allow', true) }, T(_('Allow'))));
			}
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, [ E('strong', {}, T(p.name)), ' ', E('small', {}, T(p.version)), E('br'), E('small', {}, T(p.description)) ]),
				E('td', { 'class': 'td' }, T(p.kind)),
				E('td', { 'class': 'td' }, [ E('strong', {}, T(p.state)), p.why ? E('br') : '', p.why ? E('small', {}, T(p.why)) : '' ]),
				E('td', { 'class': 'td' }, [ T(from), E('br'), E('small', {}, T(p.trusted ? _('trusted') : _('privileged capabilities need your OK'))),
					p.signature ? E('br') : '', p.signature ? E('small', {}, T(p.signature)) : '' ]),
				E('td', { 'class': 'td right' }, btns)
			]);
		});

		return E('table', { 'class': 'table' }, [ th([ _('Plug-in'), _('Kind'), _('State'), _('From'), '' ]) ]
			.concat(rows.length ? rows : [ E('tr', { 'class': 'tr placeholder' }, E('td', { 'class': 'td' }, T(_('No plug-ins.')))) ]));
	},

	models: function(m) {
		var self = this;
		var rows = (m.models || []).map(function(e) {
			var what = !e.loaded ? (e.kind != 'model' ? _('none yet (comes from cortex)') : _('not loaded'))
				: e.kind == 'list' ? _('version %d, %d names').format(e.version, e.entries)
				: e.kind == 'layer' ? _('version %d, shared layer learned by homes together').format(e.version)
				: e.source == 'package' ? _('packaged, %d trees').format(e.trees)
				: _('version %d, %d trees').format(e.version, e.trees);
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, E('strong', {}, T(e.name))),
				E('td', { 'class': 'td' }, [ T(what), e.held_back ? E('br') : '',
					e.held_back ? E('small', {}, T(_('version %d and older held back').format(e.held_back))) : '' ]),
				E('td', { 'class': 'td' }, T(e.signer ? _('%s, signed by %s').format(e.origin, e.signer) : (e.origin || '-'))),
				E('td', { 'class': 'td' }, e.refused ? E('small', {}, T(e.refused)) : T('-')),
				E('td', { 'class': 'td right' }, e.source == 'update'
					? E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'rollback', e.name) }, T(_('Roll back'))) : '')
			]);
		});
		var keys = (m.trusted_keys || []).map(function(k) { return k.fingerprint + ' (' + k.comment + ')'; });

		return E('div', {}, [
			E('table', { 'class': 'table' }, [ th([ _('Model'), _('In use'), _('From'), _('Last refused'), '' ]) ].concat(rows)),
			E('p', { 'class': 'cbi-section-descr' }, T(keys.length ? _('Trusted keys: %s').format(keys.join(', '))
				: _('No trusted keys: model updates are refused until you trust one (agctl trust <key.pub>).')))
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
			E('h2', {}, T(_('Plug-ins and models'))),
			E('div', { 'class': 'cbi-section' }, [ E('h3', {}, T(_('Plug-ins'))),
				E('p', { 'class': 'cbi-section-descr' }, T(_('Plug-ins wire themselves in. Ones you added need your OK for privileged capabilities, unless they are signed by a key you trust.'))),
				this.pbox ]),
			E('div', { 'class': 'cbi-section' }, [ E('h3', {}, T(_('Models and lists'))), this.mbox ])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
