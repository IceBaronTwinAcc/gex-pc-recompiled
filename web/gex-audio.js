addToLibrary({
  gex_web_audio_enable_limiter__deps: ['$AL'],
  gex_web_audio_enable_limiter__proxy: 'sync',
  gex_web_audio_enable_limiter: () => {
    const ctx = AL.currentCtx;
    if (!ctx || !ctx.audioCtx || !ctx.gain) {
      console.error('[audio] Emscripten OpenAL output graph is unavailable');
      return 0;
    }
    try {
      const headroom = ctx.audioCtx.createGain();
      headroom.gain.value = 0.375;
      const compressor = ctx.audioCtx.createDynamicsCompressor();
      compressor.threshold.value = -1;
      compressor.knee.value = 0;
      compressor.ratio.value = 20;
      compressor.attack.value = 0;
      compressor.release.value = 0.015;
      const makeup = ctx.audioCtx.createGain();
      // Cancel the compressor's automatic makeup gain below its threshold.
      makeup.gain.value = Math.pow(10, -0.6 * (1 - 1 / 20) / 20);
      const guard = ctx.audioCtx.createWaveShaper();
      guard.curve = new Float32Array([-0.98, 0.98]);
      ctx.gain.connect(headroom);
      headroom.connect(compressor);
      compressor.connect(makeup);
      makeup.connect(guard);
      guard.connect(ctx.audioCtx.destination);
      ctx.gain.disconnect(ctx.audioCtx.destination);
      return 1;
    } catch (error) {
      console.error('[audio] Cannot protect the browser audio output:', error);
      return 0;
    }
  },
});
