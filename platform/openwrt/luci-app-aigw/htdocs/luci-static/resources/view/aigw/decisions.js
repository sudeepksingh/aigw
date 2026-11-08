'use strict';
'require view';
'require rpc';
'require ui';
'require poll';

var callAlerts = rpc.declare({ object: 'ag.alert', method: 'list', params: [ 'limit' ], expect: { alerts: [] } });
var callDecisions = rpc.declare({ object: 'ag.decision', method: 'list', params: [ 'limit' ], expect: { decisions: [] } });
var callApprove = rpc.declare({ object: 'ag.decision', method: 'approve', params: [ 'id' ] });
var callReject = rpc.declare({ object: 'ag.decision', method: 'reject', params: [ 'id' ] });
var callRevert = rpc.declare({ object: 'ag.decision', method: 'revert', params: [ 'id' ] });

function when(ts) {
	return ts ? new Date(ts * 1000).toLocaleString() : '-';
}

function left(ts) {
	var s = ts - Math.floor(Date.now() / 1000);
	return s > 0 ? _('%t left').format(s) : '';
}

return view.extend({
	load: function() {
		return Promise.all([ callAlerts(30), callDecisions(30) ]);
	},

	change: function(fn, id) {
		var self = this;

		return fn(id).then(function(res) {
			if (res && res.ok === false)
				ui.addNotification(null, E('p', res.error || _('Failed')), 'danger');
			return self.load().then(function(d) { self.refresh(d); });
		});
	},

	decisions: function(list) {
		var self = this;
		var rows = list.map(function(d) {
			var btns = [];

			if (d.state == 'proposed') {
				btns.push(E('button', { 'class': 'btn cbi-button-positive', 'click': ui.createHandlerFn(self, 'change', callApprove, d.id) }, _('Approve')), ' ');
				btns.push(E('button', { 'class': 'btn cbi-button-negative', 'click': ui.createHandlerFn(self, 'change', callReject, d.id) }, _('Reject')));
			} else if (d.state == 'active') {
				btns.push(E('button', { 'class': 'btn cbi-button', 'click': ui.createHandlerFn(self, 'change', callRevert, d.id) }, _('Undo')));
			}
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, '#%d'.format(d.id)),
				E('td', { 'class': 'td' }, [ E('strong', d.state), E('br'), E('small', d.state == 'active' ? left(d.expires) : '') ]),
				E('td', { 'class': 'td' }, [ d.action, ' ', E('em', d.device || d.target) ]),
				E('td', { 'class': 'td' }, [ d.reason, d.note ? E('br') : '', d.note ? E('small', d.note) : '', E('br'),
					E('small', _('from %s, %s').format(d.origin, when(d.created))) ]),
				E('td', { 'class': 'td right' }, btns)
			]);
		});

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, '#'), E('th', { 'class': 'th' }, _('State')), E('th', { 'class': 'th' }, _('Action')),
				E('th', { 'class': 'th' }, _('Why')), E('th', { 'class': 'th' })
			])
		].concat(rows.length ? rows : [ E('tr', { 'class': 'tr placeholder' }, E('td', { 'class': 'td' }, _('No decisions yet.'))) ]));
	},

	alerts: function(list) {
		var rows = list.map(function(a) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, [ E('strong', a.kind), a.count > 1 ? E('small', ' ×%d'.format(a.count)) : '' ]),
				E('td', { 'class': 'td' }, a.device),
				E('td', { 'class': 'td' }, a.reason),
				E('td', { 'class': 'td' }, when(a.last))
			]);
		});

		return E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Alert')), E('th', { 'class': 'th' }, _('Device')),
				E('th', { 'class': 'th' }, _('What and why')), E('th', { 'class': 'th' }, _('Last'))
			])
		].concat(rows.length ? rows : [ E('tr', { 'class': 'tr placeholder' }, E('td', { 'class': 'td' }, _('Nothing to report.'))) ]));
	},

	refresh: function(data) {
		this.dbox.replaceChildren(this.decisions(data[1]));
		this.abox.replaceChildren(this.alerts(data[0]));
	},

	render: function(data) {
		var self = this;

		this.dbox = E('div', {});
		this.abox = E('div', {});
		this.refresh(data);
		poll.add(function() { return self.load().then(function(d) { self.refresh(d); }); }, 10);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', _('Alerts and decisions')),
			E('div', { 'class': 'cbi-map-descr' },
				_('Every action has a time limit and an undo. Your answers teach AIGW which alerts were right.')),
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Decisions')), this.dbox ]),
			E('div', { 'class': 'cbi-section' }, [ E('h3', _('Alerts')), this.abox ])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
