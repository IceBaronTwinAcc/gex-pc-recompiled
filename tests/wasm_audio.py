#!/usr/bin/env python3
"""Verify the browser's real OpenAL output graph without audible output."""

import argparse
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from playwright.sync_api import sync_playwright

from wasm_coi import Handler, PREFIX, run_game


PROBE = r"""(() => {
  const connect = AudioNode.prototype.connect;
  const disconnect = AudioNode.prototype.disconnect;
  const redirected = new WeakMap();
  const silenced = new WeakMap();
  const upstream = new WeakMap();
  const stats = () => ({frames: 0, peak: 0, clipped: 0, channels: [0, 0],
                         sine: 0, cosine: 0});
  const probe = window.gexAudioProbe = {pre: stats(), post: stats(), inputs: new Set()};
  probe.reset = () => { probe.pre = stats(); probe.post = stats(); };
  function meter(source, side) {
    const node = source.context.createScriptProcessor(1024, 2, 2);
    const sink = source.context.createMediaStreamDestination();
    node.onaudioprocess = event => {
      if (side === 'post' && node !== probe.activePost) return;
      const current = probe[side];
      if (current.frames >= 20000) return;
      const start = current.frames;
      current.frames += event.inputBuffer.length;
      for (let channel = 0; channel < event.inputBuffer.numberOfChannels; channel++) {
        let frame = 0;
        for (const sample of event.inputBuffer.getChannelData(channel)) {
          const value = Math.abs(sample);
          current.peak = Math.max(current.peak, value);
          current.channels[channel] = Math.max(current.channels[channel], value);
          if (value > 1) current.clipped++;
          if (channel === 0) {
            const phase = 2 * Math.PI * 220 * (start + frame) / source.context.sampleRate;
            current.sine += sample * Math.sin(phase);
            current.cosine += sample * Math.cos(phase);
          }
          frame++;
        }
      }
    };
    connect.call(source, node);
    connect.call(node, sink);
    return node;
  }
  AudioNode.prototype.connect = function(destination, ...args) {
    if (destination instanceof DynamicsCompressorNode) {
      probe.master = upstream.get(this) || this;
      meter(probe.master, 'pre');
    }
    if (destination === probe.master) {
      if (probe.isolate && !probe.allowSignal) {
        const silent = this.context.createGain();
        connect.call(this, silent);
        silenced.set(this, silent);
        return destination;
      }
      probe.inputs.add(this);
    }
    if (destination instanceof AudioDestinationNode) {
      const node = meter(this, 'post');
      probe.activePost = node;
      redirected.set(this, node);
      return destination;
    }
    upstream.set(destination, this);
    return connect.call(this, destination, ...args);
  };
  AudioNode.prototype.disconnect = function(destination, ...args) {
    if (!arguments.length) {
      probe.inputs.delete(this);
      silenced.delete(this);
      redirected.delete(this);
      return disconnect.call(this);
    }
    if (destination === probe.master && silenced.has(this)) {
      const silent = silenced.get(this);
      silenced.delete(this);
      return disconnect.call(this, silent);
    }
    if (destination instanceof AudioDestinationNode && redirected.has(this)) {
      const node = redirected.get(this);
      disconnect.call(this, node);
      disconnect.call(node);
      redirected.delete(this);
      return;
    }
    if (destination === probe.master) probe.inputs.delete(this);
    return disconnect.call(this, destination, ...args);
  };
})();"""


def run_signal(page, gains, settle=True, pan=None):
    if not settle:
        page.evaluate("gexAudioProbe.reset()")
    page.evaluate("""({gains, pan}) => {
      const probe = window.gexAudioProbe;
      const context = probe.master.context;
      const start = context.currentTime + 0.05;
      probe.signals = gains.map(input => {
        const {gain, frequency} = typeof input === 'number'
          ? {gain: input, frequency: 440} : input;
        const oscillator = context.createOscillator();
        const level = context.createGain();
        oscillator.frequency.value = frequency;
        level.gain.value = gain;
        oscillator.connect(level);
        const output = pan === null ? level : context.createStereoPanner();
        if (pan !== null) {
          output.pan.value = pan;
          level.connect(output);
        }
        probe.allowSignal = true;
        output.connect(probe.master);
        probe.allowSignal = false;
        oscillator.start(start);
        return {oscillator, output};
      });
    }""", {"gains": gains, "pan": pan})
    if settle:
        page.wait_for_timeout(500)
        page.evaluate("gexAudioProbe.reset()")
    page.wait_for_function(
        "() => gexAudioProbe.pre.frames >= 16000 && gexAudioProbe.post.frames >= 16000",
        timeout=10000,
    )
    result = page.evaluate("({pre: gexAudioProbe.pre, post: gexAudioProbe.post})")
    page.evaluate("""() => {
      for (const signal of gexAudioProbe.signals) {
        signal.oscillator.stop();
        signal.output.disconnect();
      }
    }""")
    return result


def music_level(result):
    output = result["post"]
    return 2 * (output["sine"] ** 2 + output["cosine"] ** 2) ** 0.5 / output["frames"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-dir", type=Path, default=Path("build/wasm/web"))
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    args = parser.parse_args()
    web_dir = args.web_dir.resolve()
    game_dir = args.game_dir.resolve()
    for path in (web_dir / "gex_recomp.html", web_dir / "gex-coi-serviceworker.js", game_dir / "GEX.exe"):
        if not path.is_file():
            parser.error(f"required file not found: {path}")

    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(web_dir)))
    server.requests, server.fail_worker, server.fail_runtime, server.isolated = [], False, False, False
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}{PREFIX}gex_recomp.html"
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True, args=[
                "--mute-audio", "--disable-webgpu", "--autoplay-policy=no-user-gesture-required"
            ])
            try:
                page = browser.new_page()
                page.add_init_script(PROBE)
                page.goto(url)
                run_game(page, game_dir, "software")
                page.wait_for_function("() => !!gexAudioProbe.master", timeout=30000)
                page.wait_for_function("() => gexAudioProbe.master.context.state === 'running'", timeout=30000)
                log = page.locator("#output").text_content()
                assert "[audio] browser output limiter enabled" in log, log
                page.evaluate("""() => {
                  const probe = gexAudioProbe;
                  probe.isolate = true;
                  for (const node of probe.inputs) node.disconnect(probe.master);
                }""")
                quiet = run_signal(page, [0.125])
                assert 0.12 < quiet["pre"]["peak"] < 0.13, quiet
                assert 0.045 < quiet["post"]["peak"] < 0.047, quiet
                left = run_signal(page, [0.125], pan=-1)
                assert left["post"]["channels"][0] > 0.045, left
                assert left["post"]["channels"][1] < 0.001, left
                right = run_signal(page, [0.125], pan=1)
                assert right["post"]["channels"][1] > 0.045, right
                assert right["post"]["channels"][0] < 0.001, right
                music = {"gain": 0.25, "frequency": 220}
                effect = {"gain": 0.9, "frequency": 997}
                music_alone = run_signal(page, [music])
                with_effect = run_signal(page, [music, effect])
                with_two_effects = run_signal(page, [music, effect, effect])
                solo_level, mixed_level = music_level(music_alone), music_level(with_effect)
                assert solo_level > 0.04 and 0.95 < mixed_level / solo_level < 1.05, (
                    solo_level, mixed_level, music_alone, with_effect
                )
                two_effects_level = music_level(with_two_effects)
                assert 0.95 < two_effects_level / solo_level < 1.05, (
                    solo_level, two_effects_level, music_alone, with_two_effects
                )
                transient = run_signal(page, [0.8, 0.8], settle=False)
                assert transient["pre"]["peak"] > 1.5, transient
                assert transient["post"]["peak"] <= 0.981 and transient["post"]["clipped"] == 0, transient
                loud = run_signal(page, [0.8, 0.8])
                assert loud["pre"]["peak"] > 1.5 and loud["pre"]["clipped"] > 0, loud
                assert 0.3 < loud["post"]["peak"] <= 0.981 and loud["post"]["clipped"] == 0, loud
                assert 0.36 < loud["post"]["peak"] / loud["pre"]["peak"] < 0.38, loud
                extreme = run_signal(page, [4, 4], settle=False)
                assert extreme["pre"]["peak"] > 7.5, extreme
                assert extreme["post"]["peak"] <= 0.981 and extreme["post"]["clipped"] == 0, extreme
                print("music unaffected by effects: PASS", solo_level, mixed_level, two_effects_level)
                print("stereo and peak protection: PASS", quiet, transient, loud, extreme)

                page.close()
                page = browser.new_page()
                page.add_init_script("""AudioContext.prototype.createDynamicsCompressor = () => {
                  throw new Error('simulated unavailable compressor');
                };""")
                page.goto(url)
                page.wait_for_function(
                    "() => !document.querySelector('#remember-game').disabled", timeout=45000
                )
                page.locator("#remember-game").uncheck()
                page.locator("#game-folder").set_input_files(str(game_dir))
                page.wait_for_function(
                    "() => document.querySelector('#output').textContent.includes("
                    "'[audio] cannot initialize the browser output limiter')",
                    timeout=80000,
                )
                assert "[audio] browser output limiter enabled" not in page.locator("#output").text_content()
                print("unavailable browser output protection reports failure: PASS")
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
