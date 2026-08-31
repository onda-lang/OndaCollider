(
var root = "ONDA_COLLIDER_ROOT".getenv;
var pluginPath = "ONDA_COLLIDER_PLUGIN_PATH".getenv;
var serverProgram = "ONDA_COLLIDER_SERVER_PROGRAM".getenv ? "scsynth";
var fixtures;
var server;

if(root.isNil or: { pluginPath.isNil }) {
    "ONDA_COLLIDER_ROOT and ONDA_COLLIDER_PLUGIN_PATH must be set".throw;
};

if(serverProgram == "supernova") {
    Server.supernova;
} {
    Server.scsynth;
};
server = Server.local;

fixtures = root +/+ "tests" +/+ "fixtures";
if(serverProgram == "scsynth") {
    // scsynth has a true null audio backend. For supernova this value instead names a JACK
    // server, so leave its device unset and let it connect to the default JACK server.
    server.options.device = "null";
};
server.options.numInputBusChannels = 0;
server.options.numOutputBusChannels = 2;
server.options.blockSize = 64;
server.options.ugenPluginsPath = "/usr/lib/SuperCollider/plugins:" ++ pluginPath;

SystemClock.sched(15, {
    if(server.serverRunning.not) {
        "OndaCollider integration test failed: server did not boot".error;
        1.exit;
    };
    nil
});

server.waitForBoot {
    Routine({
        var assert, assertClose, compile, readBus;
        var ids, stableA, stableB;
        var aliasDef, lifetimeBus, lifetimeSynth, dualDef, dualSynth, replacementDualSynth;
        var eventDef, eventBus, eventSynth;
        var typedDef, typedBus, typedSynth, loggingBus, loggingSynth;
        var longPrintDef, longPrintBus, longPrintSynth, longPrintSource;
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
            });
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

            lifetimeBus = Bus.control(server, 1);
            SynthDef(\ondaInstanceLifetimeTest, { |resultBus = 0|
                Out.kr(resultBus, A2K.kr(Mix.fill(40, { Onda.ar(\sourceAlias) })));
            }).add;
            server.sync;
            lifetimeSynth = Synth(\ondaInstanceLifetimeTest, [\resultBus, lifetimeBus.index]);
            0.1.wait;
            assertClose.(readBus.(lifetimeBus), 10.0, "each UGen should own an independently allocated Onda instance");
            aliasDef.free(server);
            server.sync;
            0.1.wait;
            assertClose.(readBus.(lifetimeBus), 0.0, "freeing a definition should detach and destroy every live UGen instance");
            lifetimeSynth.free;

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

            typedDef = compile.(\typedControls, fixtures +/+ "typed_controls.onda");
            assert.(typedDef.ins.size == 8, "typed controls should expose four params and four events");
            4.do { |index|
                assert.(typedDef.ins[index][\meta][\kind] == \param, "typed scalar params should be accepted");
            };
            4.do { |index|
                assert.(typedDef.ins[index + 4][\meta][\kind] == \event, "typed scalar events should be accepted");
            };
            assertClose.(typedDef.ins[0][\meta][\init], 1.5, "f64 param defaults should cross the SC metadata boundary");
            assertClose.(typedDef.ins[1][\meta][\init], 2.0, "i32 param defaults should cross the SC metadata boundary");
            assertClose.(typedDef.ins[2][\meta][\init], 3.0, "i64 param defaults should cross the SC metadata boundary");
            assertClose.(typedDef.ins[3][\meta][\init], 0.0, "bool param defaults should cross the SC metadata boundary");

            loggingBus = Bus.control(server, 1);
            SynthDef(\ondaLoggingMemoryTest, { |resultBus = 0|
                Out.kr(resultBus, A2K.kr(Mix.fill(40, { Onda.ar(\typedControls) })));
            }).add;
            server.sync;
            loggingSynth = Synth(\ondaLoggingMemoryTest, [\resultBus, loggingBus.index]);
            0.1.wait;
            assertClose.(readBus.(loggingBus), 260.0, "logging-enabled UGens should have bounded RT memory overhead");
            loggingSynth.free;
            server.sync;
            loggingBus.free;

            typedBus = Bus.control(server, 1);
            SynthDef(\ondaTypedControlsTest, {
                |resultBus = 0, doubleValue = 1.5, intValue = 2, longValue = 3, enabled = 0,
                addDouble = 0, addInt = 0, addLong = 0, addBool = 0|
                var output = Onda.ar(\typedControls, (
                    double_value: doubleValue,
                    int_value: intValue,
                    long_value: longValue,
                    enabled: enabled,
                    add_double: addDouble,
                    add_int: addInt,
                    add_long: addLong,
                    add_bool: addBool
                ));
                Out.kr(resultBus, A2K.kr(output));
            }).add;
            server.sync;
            typedSynth = Synth(\ondaTypedControlsTest, [
                \resultBus, typedBus.index,
                \doubleValue, 2.5,
                \intValue, 4,
                \longValue, 5,
                \enabled, 0.75
            ]);
            0.1.wait;
            assertClose.(readBus.(typedBus), 19.5, "init should observe converted SC param controls");

            typedSynth.set(\doubleValue, 1.5, \intValue, 2.75, \longValue, 3.75, \enabled, 0.25);
            0.1.wait;
            assertClose.(readBus.(typedBus), 6.5, "non-f32 scalar params should convert from SC controls");

            typedSynth.set(\addDouble, 2.5);
            0.1.wait;
            assertClose.(readBus.(typedBus), 9.0, "f64 event payloads should convert from SC controls");
            typedSynth.set(\addInt, 3.75);
            0.1.wait;
            assertClose.(readBus.(typedBus), 12.0, "i32 event payloads should truncate SC controls");
            typedSynth.set(\addLong, 4.75);
            0.1.wait;
            assertClose.(readBus.(typedBus), 16.0, "i64 event payloads should truncate SC controls");
            typedSynth.set(\addBool, 0.25);
            0.1.wait;
            assertClose.(readBus.(typedBus), 32.0, "positive bool event edges should convert to true");

            longPrintSource = "outs:\n  out1\n\ninit:\n  print(\""
                ++ String.fill(9000, { $x })
                ++ "\")\n\nsample:\n  out1 = 0.125\n";
            longPrintDef = compile.(\longPrint, longPrintSource);
            longPrintBus = Bus.control(server, 1);
            SynthDef(\ondaLongPrintTest, { |resultBus = 0|
                Out.kr(resultBus, A2K.kr(Onda.ar(\longPrint)));
            }).add;
            server.sync;
            longPrintSynth = Synth(\ondaLongPrintTest, [\resultBus, longPrintBus.index]);
            0.1.wait;
            assertClose.(readBus.(longPrintBus), 0.125, "oversized print output should be dropped safely");
            longPrintSynth.free;
            longPrintBus.free;

            invalidDef = OndaDef(\eventEdge, fixtures +/+ "invalid.onda");
            invalidDef.send(server);
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

            projectSynth.set(\bufferNumber, overrideBuffer.bufnum);
            0.1.wait;
            assertClose.(readBus.(projectBus), 7.75, "the project buffer should accept a second SC override");
            projectSynth.free;
            server.sync;
            projectSynth = Synth(\ondaProjectDefaultTest, [\resultBus, projectBus.index]);
            0.1.wait;
            assertClose.(readBus.(projectBus), 7.25, "a fresh UGen instance should start with project buffer defaults");

            arrayDef = compile.(\projectArray, fixtures +/+ "project_array" +/+ "project.ondaproject");
            assert.(arrayDef.ins.isEmpty, "project buffer arrays must not expose an SC control");
            badArrayDef = OndaDef(\plainArray, fixtures +/+ "buffer_array.onda");
            badArrayDef.send(server);
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
            typedSynth.free;
            projectSynth.free;
            requiredSynth.free;
            eventBus.free;
            lifetimeBus.free;
            typedBus.free;
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
