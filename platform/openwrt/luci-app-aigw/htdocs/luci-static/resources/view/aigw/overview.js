'use strict';
'require view';
'require rpc';
'require poll';

var callStatus = rpc.declare({ object: 'ag', method: 'status', expect: { '': {} } });
var callModels = rpc.declare({ object: 'ag.model', method: 'list', expect: { '': {} } });

function row(label, value, hint) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '33%' }, label),
		E('td', { 'class': 'td left' }, [ value, hint ? E('span', { 'class': 'cbi-value-description' }, [ ' ', hint ]) : '' ])
	]);
}

function modeText(mode) {
	switch (mode) {
	case 'enforce': return _('policies act on their own, within guardrails');
	case 'off': return _('alerts only');
	default: return _('actions wait for your approval');
	}
}

return view.extend({
	load: function() {
		return Promise.all([ callStatus(), L.resolveDefault(callModels(), {}) ]);
	},

	table: function(st, models) {
		var d = st.decisions || {}, p = st.plugins || {}, r = st.remote || {}, s = st.sensors || {};
		var mods = (models.models || []).filter(function(m) { return m.loaded; }).map(function(m) {
			return m.kind == 'list'
				? _('%s v%d (%d names)').format(m.name, m.version, m.entries)
				: m.version ? _('%s v%d (%d trees)').format(m.name, m.version, m.trees)
				: _('%s (packaged, %d trees)').format(m.name, m.trees);
		});

		return E('table', { 'class': 'table' }, [
			row(_('Version'), '%s on %s'.format(st.version, st.host), _('phase: %s').format(st.phase)),
			row(_('Running for'), '%t'.format(st.uptime_s || 0)),
			row(_('Devices'), '%d'.format(st.devices || 0), _('%d on Wi-Fi now').format(st.wifi_stations || 0)),
			row(_('DNS lookups'), '%d'.format(st.dns_queries || 0)),
			row(_('Connections'), '%d'.format(st.flows || 0), _('%d without a DNS lookup').format(st.flows_no_dns || 0)),
			row(_('Sensors'), [ s.dns ? _('DNS on') : _('DNS waiting'), ', ', s.flow ? _('flows on') : _('flows OFF'), ', ',
				s.wifi ? _('Wi-Fi on') : _('no Wi-Fi stations') ].join('')),
			row(_('Alerts'), '%d'.format(st.alerts || 0)),
			row(_('Acting'), st.mode || 'advise', modeText(st.mode)),
			row(_('Decisions'), E('a', { 'href': L.url('admin/aigw/decisions') },
				_('%d waiting for you').format(d.waiting || 0)),
				_('%d in effect, %d policies').format(d.active || 0, d.policies || 0)),
			row(_('Plug-ins'), _('%d found, %d working').format(p.found || 0, p.working || 0)),
			row(_('Wi-Fi keys'), _('%d per-device keys').format(st.wifi_keys || 0)),
			row(_('Remote access'), r.enabled ? _('on, %d peers, %d connected').format(r.peers || 0, r.online || 0) : _('off')),
			row(_('Models'), mods.length ? mods.join(', ') : _('none loaded'))
		]);
	},

	render: function(data) {
		var box = E('div', {}, this.table(data[0], data[1]));
		var self = this;

		poll.add(function() {
			return Promise.all([ callStatus(), L.resolveDefault(callModels(), {}) ]).then(function(res) {
				box.replaceChildren(self.table(res[0], res[1]));
			});
		}, 10);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', _('AIGW')),
			E('div', { 'class': 'cbi-map-descr' },
				_('The AI-native gateway: what is on your network, what it does, and what AIGW thinks you should know.')),
			E('div', { 'class': 'cbi-section' }, box)
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
