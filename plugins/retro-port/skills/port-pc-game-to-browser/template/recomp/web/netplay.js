// Netplay transport, shared by the page (port.js) and the headless Node harness (node_env.js).
//
// Connects to a room on the Worker (worker.js: NetplayRoom), shows the lobby through callbacks, and once the
// host starts the session moves lockstep messages between this peer's game (host/netplay.c) and the room:
// it drains the game's outbox (our pad inputs, state hashes) into the socket and feeds what other peers
// send into the game. Plain script: the page loads it with <script>, Node with require().
(function (root) {
  'use strict';


  // Room client. opts: { url (ws(s)://.../netplay/<CODE>?ticket=...), name, version, iceServers (STUN/TURN
  // for the direct channels, from the ticket), relayOnly (direct channels only through TURN), testLoss (tests:
  // drop this fraction of direct packets on arrival), onLobby(lobby),
  // onStart(session), onLeft(slot), onError(message), onClose(), reconnectUrl() (async: a fresh url with a
  // new ticket), onReconnect(state) }. If the room connection drops during a game it reconnects, rejoins
  // its slot and resends its recent inputs (the others wait in lockstep meanwhile).
  //
  // Inputs go through the room (a relay every peer can reach) and, where WebRTC is available, also over a
  // direct data channel to each peer, set up through the room in the lobby. The game stores each frame's
  // input idempotently, so duplicates are harmless and the faster path wins. The direct channels don't
  // retransmit (a resend would come long after the room's copy); instead every packet also repeats our last
  // few inputs, so a lost one is covered by the next, a frame later. Round trips to the room and
  // over each direct channel are measured and reported, so the lobby can pick an input delay that covers
  // the slowest pair of players (autoDelay).
  function connect(opts) {
    let ws = null;
    let started = false, closing = false, retries = 0, heard = Date.now();
    const history = [];       // our recent input batches, resent after a reconnect
    const HISTORY = 600;
    const recent = [];        // our last few inputs, repeated in every direct packet
    const REPEAT = 8;
    let game = null;          // the Emscripten module once attached
    let timer = null;
    let me = -1;
    const early = [];         // input batches that arrive before the game is attached
    let rtt = 0, reported = 0;
    const peers = new Map();  // slot -> { pc, dc, rtt }
    let p2pReported = '';
    const send = (m) => { if (ws && ws.readyState === 1) ws.send(JSON.stringify(m)); };
    const ping = setInterval(() => {
      // in a game, a room connection that has gone quiet (pongs come every second) is dead even if the
      // browser hasn't noticed yet: drop it and reconnect
      if (started && !closing && ws && ws.readyState === 1 && Date.now() - heard > 5000) abandon();
      send({ t: 'ping', ts: Date.now() });
      for (const p of peers.values()) if (p.dc?.readyState === 'open') p.dc.send(JSON.stringify({ t: 'ping', ts: Date.now() }));
      const p2p = {};
      for (const [slot, p] of peers) if (p.rtt) p2p[slot] = Math.round(p.rtt);
      const key = JSON.stringify(p2p);
      if (key !== p2pReported) { p2pReported = key; send({ t: 'p2p', rtt: p2p }); }
    }, 1000);

    function open(url, rejoin) {
      ws = new WebSocket(url);
      heard = Date.now();
      ws.onopen = () => {
        retries = 0;
        heard = Date.now();
        if (rejoin) send({ t: 'rejoin', slot: me, version: opts.version });
        else send({ t: 'hello', name: opts.name, version: opts.version });
        send({ t: 'ping', ts: Date.now() });
      };
      ws.onclose = () => {
        if (started && !closing) { reconnect(); return; }
        if (timer) clearInterval(timer);
        clearInterval(ping);
        for (const p of peers.values()) p.pc?.close();
        opts.onClose?.();
      };
      ws.onerror = () => { if (!started) opts.onError?.('Connection to the game room failed.'); };
      ws.onmessage = (ev) => { heard = Date.now(); onSocketMessage(ev); };
    }
    function abandon() {
      const old = ws;
      old.onclose = old.onmessage = old.onerror = null;
      try { old.close(); } catch { /* already gone */ }
      reconnect();
    }
    async function reconnect() {
      opts.onReconnect?.('connecting');
      await new Promise((r) => setTimeout(r, Math.min(5000, 500 * 2 ** retries++)));
      try { open(opts.reconnectUrl ? await opts.reconnectUrl() : opts.url, true); }
      catch { reconnect(); }
    }

    function onMessage(m, from) {
      if (from && opts.testLoss && Math.random() < opts.testLoss) return;
      if (m.t === 'in') { if (game) deliver(game, m); else early.push(m); }
      else if (m.t === 'ping' && from) from.dc.send(JSON.stringify({ t: 'pong', ts: m.ts }));
      else if (m.t === 'pong') {
        const ms = Date.now() - m.ts;
        if (from) from.rtt = from.rtt ? from.rtt * 0.7 + ms * 0.3 : ms;
        else {
          rtt = rtt ? rtt * 0.7 + ms * 0.3 : ms;
          if (Math.abs(rtt - reported) > 10 || !reported) { reported = rtt; send({ t: 'rtt', ms: rtt }); }
        }
      }
    }

    // ---- direct channels (browser only)
    const webrtc = typeof RTCPeerConnection !== 'undefined';
    function peer(slot) {
      let p = peers.get(slot);
      if (p) return p;
      const pc = new RTCPeerConnection({ iceServers: opts.iceServers || [], iceTransportPolicy: opts.relayOnly ? 'relay' : 'all' });
      p = { pc, dc: null, rtt: 0 };
      peers.set(slot, p);
      const use = (dc) => {
        p.dc = dc;
        dc.onopen = () => describePath(slot, pc);
        dc.onmessage = (ev) => { try { onMessage(JSON.parse(ev.data), p); } catch { /* bad frame */ } };
      };
      pc.onicecandidate = (ev) => { if (ev.candidate) send({ t: 'sig', to: slot, data: { candidate: ev.candidate.toJSON() } }); };
      pc.ondatachannel = (ev) => use(ev.channel);
      if (me < slot) {   // the lower slot makes the offer
        use(pc.createDataChannel('inputs', { ordered: false, maxRetransmits: 0 }));
        pc.createOffer().then((o) => pc.setLocalDescription(o)).then(() => send({ t: 'sig', to: slot, data: { sdp: pc.localDescription.toJSON() } }));
      }
      return p;
    }
    // Logs how a direct channel connected: host/srflx = straight between the players, relay = through TURN.
    async function describePath(slot, pc) {
      try {
        const stats = await pc.getStats();
        let pair = null;
        stats.forEach((r) => { if (r.type === 'transport' && r.selectedCandidatePairId) pair = stats.get(r.selectedCandidatePairId); });
        stats.forEach((r) => { if (!pair && r.type === 'candidate-pair' && r.nominated && r.state === 'succeeded') pair = r; });
        const local = pair && stats.get(pair.localCandidateId), remote = pair && stats.get(pair.remoteCandidateId);
        console.log(`[netplay] direct channel to player ${slot + 1}: ${local?.candidateType || '?'} -> ${remote?.candidateType || '?'}`);
      } catch (e) { console.warn('[netplay] getStats:', e); }
    }
    async function onSignal(from, data) {
      const { pc } = peer(from);
      if (data.sdp) {
        await pc.setRemoteDescription(data.sdp);
        if (data.sdp.type === 'offer') {
          await pc.setLocalDescription(await pc.createAnswer());
          send({ t: 'sig', to: from, data: { sdp: pc.localDescription.toJSON() } });
        }
      } else if (data.candidate) await pc.addIceCandidate(data.candidate).catch(() => {});
    }

    function onSocketMessage(ev) {
      let m;
      try { m = JSON.parse(ev.data); } catch { return; }
      if (m.t === 'lobby') {
        me = m.you;
        if (webrtc) {
          const present = new Set(m.players.map((p) => p.slot));
          for (const [slot, p] of peers) if (!present.has(slot)) { p.pc.close(); peers.delete(slot); }
          for (const p of m.players) if (p.slot > me) peer(p.slot);
        }
        opts.onLobby?.(m);
      } else if (m.t === 'start') { started = true; opts.onStart?.(m); }
      else if (m.t === 'rejoined') {   // back in the game: resend what may have been lost
        for (const text of history) ws.send(text);
        opts.onReconnect?.('rejoined');
      }
      else if (m.t === 'error') opts.onError?.(m.msg);
      else if (m.t === 'sig' && webrtc) onSignal(m.from, m.data).catch((e) => console.warn('webrtc:', e));
      else if (m.t === 'left') { if (game) game._netplay_player_left(m.slot); opts.onLeft?.(m.slot); }
      else onMessage(m, null);
    }

    function deliver(M, m) {
      for (const [s, f, b, lx, ly, rx, ry, dt] of m.d || []) M._netplay_remote_input(s, f, b, lx, ly, rx, ry, dt >>> 0);
      for (const [s, f, lo, hi] of m.h || []) M._netplay_remote_hash(s, f, lo, hi);
    }

    // Drains the game's outbox to the room and every open direct channel.
    function pump() {
      const d = [], h = [];
      while (game._netplay_pop()) {
        const v = [];
        for (let i = 0; i < 10; i++) v.push(game._netplay_msg(i));
        if (v[0] === 1) d.push([...v.slice(1, 8), v[8] >>> 0]);   // input: slot, frame, buttons, axes, frame time (us)
        else if (v[0] === 2) h.push([v[1], v[2], v[8] >>> 0, v[9] >>> 0]);   // hash: slot, frame, lo, hi
      }
      if (!d.length && !h.length) return;
      const text = JSON.stringify({ t: 'in', d, h });
      history.push(text);
      if (history.length > HISTORY) history.shift();
      if (ws.readyState === 1) ws.send(text);
      recent.push(...d);
      recent.splice(0, recent.length - REPEAT);
      const direct = JSON.stringify({ t: 'in', d: recent, h });
      for (const p of peers.values()) if (p.dc?.readyState === 'open') p.dc.send(direct);
    }

    open(opts.url, false);

    return {
      start(delay) { send({ t: 'start', delay }); },
      // Hooks a running game up to the room (after it was started with the session's settings).
      attach(M, pollMs = 2) {
        game = M;
        for (const m of early.splice(0)) deliver(M, m);
        timer = setInterval(pump, pollMs);
      },
      close() { closing = true; ws.close(); },
      testDrop() { abandon(); },   // tests: lose the room connection as a network drop would
      rtt: () => rtt,
      direct: () => [...peers.entries()].filter(([, p]) => p.dc?.readyState === 'open').map(([slot]) => slot),
    };
  }

  // Input delay (frames at 60 Hz) covering the slowest pair of players: per pair the direct round trip
  // if they have one, else their two trips to the room; one way is half a round trip, plus jitter margin.
  function autoDelay(players) {
    let oneWay = 0;
    for (const a of players)
      for (const b of players) {
        if (a.slot >= b.slot) continue;
        const direct = Math.max(a.p2p?.[b.slot] || 0, b.p2p?.[a.slot] || 0);
        const viaRoom = ((a.rtt || 0) + (b.rtt || 0)) / 2;
        oneWay = Math.max(oneWay, direct ? Math.min(direct / 2, viaRoom) : viaRoom);
      }
    return Math.max(2, Math.min(30, Math.ceil((oneWay * 1.25 + 20) / (1000 / 60))));
  }

  // Environment for a peer's game: deterministic simulation, this peer's slot, the session's settings.
  function gameEnv(session) {
    return {
      PORT_DETERMINISTIC: '1', PORT_NETPLAY: '1', PORT_NP_PLAYERS: String(session.players),
      PORT_NP_SLOT: String(session.you), PORT_NP_DELAY: String(session.delay), PORT_PADS: String(session.players),
    };
  }

  const api = { connect, gameEnv, autoDelay };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.PortNetplay = api;
})(typeof self !== 'undefined' ? self : this);
