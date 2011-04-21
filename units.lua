
StaticLibrary {
	Name = "common",
	Sources =  {
		"src/util.c", "src/transport.c", "src/peer.c", "src/protocol.c", "src/socket_includes.c",
		CompileNetMessages {
			Pass = "Codegen",
			Input = 'src/rlnet.msg',
			OutputStem = 'rlnet',
		}
	},
	Includes = {
		"$(OBJECTDIR)/_generated", "src",
	},
}


Program {
	Config = { "macosx-*-*", "win64-*-*" },
	Name = "rl-controller",
	Includes = {
		"$(OBJECTDIR)/_generated", "src",
	},
	Sources = {
		"src/controller.c", "src/file_server.c"
	},
	Depends = {
		"common"
	},
}

Program {
	Config = "amiga-*-*",
	Name = "rl-target",
	Libs = { "amiga" },
	Includes = {
		"$(OBJECTDIR)/_generated", "src",
	},
	Sources = {
		"src/amigafs.c", "src/target.c"
	},
	Depends = {
		"common"
	},
}

Default "rl-controller"
Default "rl-target"
