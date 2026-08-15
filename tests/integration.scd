(
var root = "ONDA_COLLIDER_ROOT".getenv;
var pluginPath = "ONDA_COLLIDER_PLUGIN_PATH".getenv;
var fixtures;
var server = Server.local;

if(root.isNil or: { pluginPath.isNil }) {
    "ONDA_COLLIDER_ROOT and ONDA_COLLIDER_PLUGIN_PATH must be set".throw;
};

fixtures = root +/+ "tests" +/+ "fixtures";
server.options.device = "null";
server.options.numInputBusChannels = 0;
server.options.numOutputBusChannels = 2;
server.options.blockSize = 64;
server.options.ugenPluginsPath = "/usr/lib/SuperCollider/plugins:" ++ pluginPath;

server.waitForBoot {
    Routine({
        var assert, assertClose, compile, readBus;
        var ids, stableA, stableB;
        var aliasDef, dualDef, dualSynth, replacementDualSynth;
        var eventDef, eventBus, eventSynth;
        var projectDef, projectBus, projectSynth, overrideBuffer, done;
        var arrayDef, badArrayDef, invalidDef;
        var requiredDef, requiredBus, requiredSynth;

        assert = { |condition, message|
            if(condition.not) { Error("OndaCollider integration test failed: " ++ message).throw };
        };
        assertClose = { |actual, expected, message|
            assert.((actual - expected).abs < 0.0001, "% (expected %, got %)".format(message, expected, actual));
        };
        compile = { |key, path|
            var result;
            OndaDef(key, path).send(server, { |definition|
                result = definition;
            }, 4);
            result
        };
        readBus = { |bus|
            var condition = Condition(false);
            var result;
            bus.get({ |value|
                result = value;
                condition.unhang;
            });
            condition.hang;
            result
        };

        try {
            ids = 1024.collect { |index| OndaDef(("unique" ++ index).asSymbol, "outs:\n  out1\nsample:\n  out1 = 0.0").id };
            assert.(ids.as(Set).size == ids.size, "definition ids must be collision-free");
            stableA = OndaDef(\stableId, "outs:\n  out1\nsample:\n  out1 = 0.0");
            stableB = OndaDef(\stableId, "outs:\n  out1\nsample:\n  out1 = 1.0");
            assert.(stableA.id == stableB.id, "a key must retain its definition id");

            aliasDef = compile.(\sourceAlias, fixtures +/+ "source_alias.on");
            assert.(aliasDef.outs == 1, ".on source files should compile through Onda's filesystem entry point");

            dualDef = compile.(\dualOsc, root +/+ "examples" +/+ "dualOsc.onda");
            assert.(dualDef.outs == 2, "dual oscillator should expose two outputs");
            dualSynth = {
                Onda.ar(\dualOsc, (freq: 220, modDepth: 2, detune: 4)) * 0.1
            }.play(server);
            server.sync;

            dualDef = compile.(\dualOsc, root +/+ "examples" +/+ "dualOsc.onda");
            replacementDualSynth = {
                Onda.ar(\dualOsc, (freq: 330, modDepth: 3, detune: 6)) * 0.1
            }.play(server);
            server.sync;
            dualSynth.free;
            replacementDualSynth.free;

            eventDef = compile.(\eventEdge, fixtures +/+ "event_edge.onda");
            assert.(eventDef.ins.size == 1, "event program should expose one control");
            assert.(eventDef.ins[0][\meta][\kind] == \event, "event metadata kind should be preserved");
            assertClose.(eventDef.ins[0][\meta][\init], 0.0, "events should default to an idle trigger");

            eventBus = Bus.control(server, 1);
            SynthDef(\ondaEventEdgeTest, { |resultBus = 0, eventValue = 0|
                Out.kr(resultBus, A2K.kr(Onda.ar(\eventEdge, (bump: eventValue))));
            }).add;
            server.sync;
            eventSynth = Synth(\ondaEventEdgeTest, [\resultBus, eventBus.index]);
            0.1.wait;
            eventSynth.set(\eventValue, 2.0);
            0.1.wait;
            assertClose.(readBus.(eventBus), 2.0, "a positive edge should trigger once");
            0.1.wait;
            assertClose.(readBus.(eventBus), 2.0, "a held-positive trigger must not retrigger each block");
            eventSynth.set(\eventValue, 0.0);
            0.05.wait;
            eventSynth.set(\eventValue, 3.0);
            0.1.wait;
            assertClose.(readBus.(eventBus), 5.0, "a new positive edge should carry its f32 payload");

            invalidDef = OndaDef(\eventEdge, fixtures +/+ "invalid.onda");
            invalidDef.send(server, nil, 1);
            0.5.wait;
            assert.(OndaDef.all[\eventEdge] === eventDef, "a failed replacement must retain the last good definition");
            assertClose.(readBus.(eventBus), 5.0, "a failed replacement must retain the running server program");

            projectDef = compile.(\projectDefaults, fixtures +/+ "project_defaults" +/+ "project.ondaproject");
            assert.(projectDef.ins.size == 1, "non-f32 project buffers should remain project-owned");
            assert.(projectDef.ins[0][\name] == \waveform, "the overridable f32 project buffer should be exposed");
            assert.(projectDef.ins[0][\meta][\kind] == \buffer, "buffer metadata kind should be preserved");
            assertClose.(projectDef.ins[0][\meta][\init], -1.0, "a project buffer should default to its project asset");

            projectBus = Bus.control(server, 1);
            SynthDef(\ondaProjectDefaultTest, { |resultBus = 0, bufferNumber = -1|
                Out.kr(resultBus, A2K.kr(Onda.ar(\projectDefaults, (waveform: bufferNumber))));
            }).add;
            server.sync;
            projectSynth = Synth(\ondaProjectDefaultTest, [\resultBus, projectBus.index]);
            0.1.wait;
            assertClose.(readBus.(projectBus), 7.25, "project-owned scalar buffers should be active by default");

            done = Condition(false);
            overrideBuffer = Buffer.loadCollection(server, Array.fill(64, 0.75), 1, { done.unhang });
            done.hang;
            projectSynth.set(\bufferNumber, overrideBuffer.bufnum);
            0.1.wait;
            assertClose.(readBus.(projectBus), 7.75, "an SC buffer should override an f32 project buffer");
            projectSynth.set(\bufferNumber, overrideBuffer.bufnum + 0.5);
            0.1.wait;
            assertClose.(readBus.(projectBus), 7.25, "a non-integral buffer id should restore the project default");
            projectSynth.set(\bufferNumber, -1);
            0.1.wait;
            assertClose.(readBus.(projectBus), 7.25, "the sentinel should restore the project default");

            arrayDef = compile.(\projectArray, fixtures +/+ "project_array" +/+ "project.ondaproject");
            assert.(arrayDef.ins.isEmpty, "project buffer arrays must not expose an SC control");
            badArrayDef = OndaDef(\plainArray, fixtures +/+ "buffer_array.onda");
            badArrayDef.send(server, nil, 1);
            0.5.wait;
            assert.(OndaDef.all[\plainArray].isNil, "plain-source buffer arrays must be rejected");

            requiredDef = compile.(\requiredBuffer, fixtures +/+ "required_buffer.onda");
            requiredBus = Bus.control(server, 1);
            SynthDef(\ondaRequiredBufferTest, { |resultBus = 0, bufferNumber = 0|
                Out.kr(resultBus, A2K.kr(Onda.ar(\requiredBuffer, (waveform: bufferNumber))));
            }).add;
            server.sync;
            requiredSynth = Synth(\ondaRequiredBufferTest, [
                \resultBus, requiredBus.index,
                \bufferNumber, overrideBuffer.bufnum
            ]);
            0.1.wait;
            assertClose.(readBus.(requiredBus), 0.75, "a required SC buffer should bind");
            requiredSynth.set(\bufferNumber, 999999);
            0.1.wait;
            assertClose.(readBus.(requiredBus), 0.0, "an invalid required buffer should silence the current block");

            eventSynth.free;
            projectSynth.free;
            requiredSynth.free;
            eventBus.free;
            projectBus.free;
            requiredBus.free;
            overrideBuffer.free;
            OndaDef.freeAll;
            server.sync;
            "OndaCollider integration tests passed".postln;
            server.quit;
            0.exit;
        } { |error|
            error.reportError;
            server.quit;
            1.exit;
        };
    }).play(AppClock);
};
)
