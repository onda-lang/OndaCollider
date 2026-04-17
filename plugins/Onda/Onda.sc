OndaDef {
	classvar <all;

	var <key;
	var <hash;
	var <numAllocate;
	var <source;
	var <tmpFile;

	var <ins;
	var <outs;

	*initClass {
		all = IdentityDictionary.new;
	}

	*new { |key, source|
		^super.new.init(key, source);
	}

	init { |argKey, argSource|
		var tmpFileCtr = 0;
		var src, srcPath, isOndaPath;

		key = argKey.asSymbol;

		hash = (key.hash.abs & 0xFFFFFF).asInteger;

		src = argSource.asString;
		srcPath = PathName(src);
		isOndaPath = srcPath.extension.contains("onda");

		if(isOndaPath) {
			var fullPath = srcPath.fullPath;
			if(File.exists(fullPath)) {
				source = fullPath;
			} {
				"Invalid path: '%'".format(fullPath).error;
				^this;
			}
		} {
			tmpFile = PathName.tmp ++ key.asString ++ tmpFileCtr.asString ++ ".onda";
			File.use(tmpFile, "w", { |f|
				f.write(src);
			});
			source = tmpFile;
		};
	}

	send { |server, action, numAllocate = 32|
		server = server ? Server.default;
		if(server.serverRunning) {
			var cond = Condition(false);
			var oscFunc = OSCFunc.newMatching({ | msg, time, addr |
				var rawStr = msg.last.asString;
				var parts = rawStr.split($/);

				if(parts[0].asSymbol == \_onda) {
					var replyHash = parts[1].asInteger;
					if(hash == replyHash) {
						var success = parts.last.asSymbol != \_fail;
						if (success) {
							var numIns = parts[2].asInteger;
							var cursor = 3;

							ins = [];

							numIns.do({
								var name = parts[cursor].asSymbol;
								var rateInt = parts[cursor + 1].asInteger;
								var rateSym;
								var meta = ();

								if (rateInt == 0) {
									rateSym = \audio;
								} {
									rateSym = \control;
								};

								cursor = cursor + 2;

								while {
									(cursor < (parts.size - 1)) && parts[cursor].asString.beginsWith("_")
								} {
									var tag = parts[cursor].asSymbol;
									var val = parts[cursor + 1];

									case
									{ tag == \_init } { meta[\init] = val.asFloat }
									{ tag == \_kind } {
										var kindInt = val.asInteger;
										meta[\kind] = if(kindInt == 0) {
											\input
										} {
											if(kindInt == 1) {
												\param
											} {
												if(kindInt == 2) {
													\event
												} {
													if(kindInt == 3) { \buffer } { \input };
												};
											};
										};
									};

									cursor = cursor + 2;
								};

								ins = ins.add((
									name: name,
									rate: rateSym,
									meta: meta
								));
							});

							outs = parts.last.asInteger;
							all.put(key, this);

							"OndaDef: Compilation of '%' succeeded.".format(key).postln;
						} {
							"OndaDef: Compilation of '%' failed.".format(key).error;
						};

						cond.unhang;
					};
				};
			}, '/done', server.addr);

			forkIfNeeded {
				var msg = ["/cmd", "onda_compile", hash, numAllocate.asInteger, source];

				server.sendMsg(*msg);
				cond.hang;

				if (File.delete(tmpFile).not, {
					"OndaDef: Could not delete temp file %".format(tmpFile).warn;
				});

				oscFunc.free;

				if(action.notNil.and(outs.notNil)) {
					action.value(this);
				}
			}

		} {
			"OndaDef: Server not running. Definition not sent.".warn;
		}
	}

	add { |server, action, numAllocate = 32|
		this.send(server, action, numAllocate);
	}

	free { |server|
		server = server ? Server.default;
		if(server.serverRunning) {
			server.sendMsg("/cmd", "onda_free", hash);
		};
		all.removeAt(key);
	}

	*free { |key|
		var def = OndaDef.all[key];
		if (def.isOndaDef) {
			def.free;
		} {
			("Invalid OndaDef to free: '" ++ key ++ "'").error;
		}
	}

	*freeAll {
		all.do { |def| def.free };
		all.clear;
	}

	asString {
		^(this.class.asString ++ "(" ++ key.asString ++ " : " ++ hash.asString ++ ")");
	}

	query {
		("\nKey: " ++ key).postln;
		("Hash: " ++ hash).postln;
		("Inputs: " ++ ins).postln;
		("Outputs: " ++ outs).postln;
	}

	isOndaDef {
		^true
	}
}

Onda : MultiOutUGen {
	*ar { |def ... args|
		var inputs = [];
		var inputMap = nil;
		var defKey = def;

		if (def.class == Symbol) {
			def = OndaDef.all[def];
		};

		if (def.isOndaDef.not) {
			"Onda: OndaDef for '%' not found.".format(defKey).warn;
			^Silent.ar(1);
		};

		if (args.size == 1 and: { args[0].isKindOf(Event) }) {
			inputMap = args[0];
		};

		def.ins.do { |in, i|
			var name = in[\name];
			var rate = in[\rate];
			var meta = in[\meta];
			var val;

			if (inputMap.notNil) {
				val = inputMap[name];
			} {
				if (i < args.size) {
					val = args[i];
				};
			};

			if (val.isNil) {
				var initVal = meta[\init];
				if (initVal.notNil) {
					val = initVal;
				} {
					"Onda '%': Input '%' missing and no init provided. Using 0.0.".format(def.key, name).warn;
					val = 0.0;
				};
			};

			if (rate == \audio) {
				var converted = false;

				if (val.isUGen) {
					if (val.rate != \audio) {
						val = K2A.ar(val);
						converted = true;
					};
				} {
					val = K2A.ar(val);
					converted = true;
				};

				if (converted) {
					"Onda '%': Converted control input '%' to audio rate.".format(def.key, name).warn;
				};
			};

			if (rate == \control) {
				if (val.isUGen) {
					if (val.rate == \audio ) {
						val = A2K.kr(val);
						"Onda '%': Converted audio input '%' to control rate.".format(def.key, name).warn;
					};
				};
			};

			inputs = inputs.add(val);
		};

		^this.multiNewList(['audio', def.hash, def.outs] ++ inputs);
	}

	init { |hash, numOutputs ... args|
		inputs = [hash] ++ args;
		^this.initOutputs(numOutputs, \audio);
	}
}

+Object {
	isOndaDef {
		^false
	}
}
