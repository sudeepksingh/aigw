'use strict';
'require view';
'require rpc';
'require poll';

var callStatus = rpc.declare({ object: 'ag', method: 'status', expect: { '': {} } });
var callModels = rpc.declare({ object: 'ag.model', method: 'list', expect: { '': {} } });
var callCache = rpc.declare({ object: 'ag.dnscache', method: 'stats', expect: { '': {} } });

function loadAll() {
	return Promise.all([ callStatus(), L.resolveDefault(callModels(), {}), L.resolveDefault(callCache(), {}) ]);
}

function cacheText(c) {
	if (!c.cache)
		return [ _('off'), '' ];
	return [ _('%d%% of lookups answered from the cache').format(c.hit_pct || 0),
		_('%s ms from the cache, %s ms from upstream; %d refreshed early, %d stale answers')
			.format(((c.hit_avg_us || 0) / 1000).toFixed(2), ((c.upstream_avg_us || 0) / 1000).toFixed(1),
				c.prefetches || 0, c.stale || 0) ];
}

/* text, never markup: names and reasons come from the network */
function T(s) {
	return document.createTextNode(s == null ? '' : String(s));
}

function row(label, value, hint) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '33%' }, T(label)),
		E('td', { 'class': 'td left' }, [ (value instanceof Node) ? value : T(value),
			hint ? E('span', { 'style': 'opacity:.65;margin-left:.6em' }, T(hint)) : '' ])
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
		return loadAll();
	},

	table: function(st, models, cache) {
		var d = st.decisions || {}, p = st.plugins || {}, r = st.remote || {}, s = st.sensors || {};
		var ct = cacheText(cache || {});
		var mods = (models.models || []).filter(function(m) { return m.loaded; }).map(function(m) {
			return m.kind == 'list'
				? _('%s v%d (%d names)').format(m.name, m.version, m.entries)
				: m.kind == 'layer' ? _('%s v%d (learned by homes together)').format(m.name, m.version)
				: m.version ? _('%s v%d (%d trees)').format(m.name, m.version, m.trees)
				: _('%s (packaged, %d trees)').format(m.name, m.trees);
		});

		return E('table', { 'class': 'table' }, [
			row(_('Version'), '%s on %s'.format(st.version, st.host), _('phase: %s').format(st.phase)),
			row(_('Running for'), '%t'.format(st.uptime_s || 0)),
			row(_('Devices'), String(st.devices || 0), _('%d home, %d on Wi-Fi now').format(st.home || 0, st.wifi_stations || 0)),
			row(_('Wi-Fi'), _('%d access points').format(st.radios || 0),
				(st.radios || 0) > 1 ? _('%d devices moved to a better band').format(st.steered || 0) : ''),
			row(_('DNS lookups'), String(st.dns_queries || 0)),
			row(_('DNS cache'), ct[0], ct[1]),
			row(_('Connections'), String(st.flows || 0), _('%d without a DNS lookup, %d named from their handshake')
				.format(st.flows_no_dns || 0, st.flows_sni || 0)),
			row(_('Sensors'), [ s.dns ? _('DNS on') : _('DNS waiting'), s.flow ? _('flows on') : _('flows OFF'),
				s.sni ? _('connection names on') : _('connection names off'),
				s.wifi ? _('Wi-Fi on') : _('no Wi-Fi stations') ].join(', ')),
			row(_('Alerts'), String(st.alerts || 0)),
			row(_('Acting'), st.mode || 'advise', modeText(st.mode)),
			row(_('Decisions'), E('a', { 'href': L.url('admin/aigw/decisions') }, T(_('%d waiting for you').format(d.waiting || 0))),
				_('%d in effect, %d policies').format(d.active || 0, d.policies || 0)),
			row(_('Plug-ins'), _('%d found, %d working').format(p.found || 0, p.working || 0)),
			row(_('Wi-Fi keys'), _('%d per-device keys').format(st.wifi_keys || 0)),
			row(_('Remote access'), r.enabled ? _('on, %d peers, %d connected').format(r.peers || 0, r.online || 0) : _('off')),
			row(_('Models'), mods.length ? mods.join(', ') : _('none loaded'))
		]);
	},

	render: function(data) {
		var box = E('div', {}, this.table(data[0], data[1], data[2]));
		var self = this;

		poll.add(function() {
			return loadAll().then(function(res) {
				box.replaceChildren(self.table(res[0], res[1], res[2]));
			});
		}, 10);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', { 'style': 'display:flex;align-items:center;gap:0.5em' }, [
				E('img', { 'src': L.resource('aigw/logo-mark.svg'), 'alt': '', 'width': 40, 'height': 40 }),
				T(_('AIGW OS'))
			]),
			E('div', { 'class': 'cbi-map-descr' },
				T(_('The AI-powered operating system for home gateways: what is on your network, what it does, and what AIGW OS thinks you should know.'))),
			E('div', { 'class': 'cbi-section' }, box)
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
