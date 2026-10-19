/* Renders sound loops off the main thread so the page never stalls when a sound is switched on.
   The page terminates this worker a few seconds after the last request, so it holds no memory. */
importScripts('dsp.js');

self.onmessage = (e) => {
  const { id, seed, rate, token } = e.data;
  const r = MurmurDSP.render(id, seed, rate);
  self.postMessage({ id, token, rate: r.rate, channels: r.channels }, r.channels.map(c => c.buffer));
};
