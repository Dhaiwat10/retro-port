Error.stackTraceLimit = 400;
// --pre-js for the Node test build: pass PORT_* environment variables to the game's getenv().
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
  ENV.PORT_HEADLESS = '1';   // no page to render to
  for (const [k, v] of Object.entries(process.env)) if (k.startsWith('PORT_')) ENV[k] = v;
});

// Netplay harness: PORT_NP_URL=ws://.../netplay/<CODE>?ticket=... joins a room (web/netplay.js) and holds
// main() back until the host starts the session; PORT_NP_HOST=<n> makes this peer the one that starts it
// once n players are in. PORT_NP_DELAY (frames, or auto: the default) is the input delay the host asks for.
if (process.env.PORT_NP_URL) {
  const np = require(require('path').join(__dirname, '../../web/netplay.js'));
  const want = Number(process.env.PORT_NP_HOST || 0);
  let started = false;
  Module.preRun.push(() => {
    addRunDependency('netplay');
    const client = np.connect({
      url: process.env.PORT_NP_URL,
      name: process.env.PORT_NP_NAME || 'node',
      version: process.env.PORT_NP_VERSION || 'test',
      onLobby(l) {
        console.error(`[netplay] lobby: you are player ${l.you}, ${l.players.length} in the room`);
        // PORT_NP_DELAY=auto (default) waits until every player has reported a round trip to the room
        const d = process.env.PORT_NP_DELAY || 'auto';
        if (want && l.host && !started && l.players.length >= want && (d !== 'auto' || l.players.every((p) => p.rtt > 0)))
          client.start(d === 'auto' ? np.autoDelay(l.players) : Number(d));
      },
      onStart(s) {
        started = true;
        console.error(`[netplay] session start: ${s.players} players, delay ${s.delay}, you are ${s.you}`);
        Object.assign(ENV, np.gameEnv(s));
        removeRunDependency('netplay');
      },
      onLeft(slot) { console.error(`[netplay] player ${slot} left`); },
      onReconnect(state) { console.error(`[netplay] room connection: ${state}`); },
      onError(e) { console.error(`[netplay] ${e}`); process.exit(2); },
    });
    const prev = Module.onRuntimeInitialized;
    Module.onRuntimeInitialized = () => {
      prev?.();
      client.attach(Module);
      // test: PORT_NP_DROP_AT=<seconds> drops this peer's room connection once, mid-game
      if (process.env.PORT_NP_DROP_AT) setTimeout(() => { console.error('[netplay] test: dropping the room connection'); client.testDrop(); }, Number(process.env.PORT_NP_DROP_AT) * 1000);
    };
  });
}
