
local CFiles = { ".c", ".h" }

local common = {
  Env = {
    CPPPATH = { "$(OBJECTDIR)" },
  },
	Defines = {
		{ "NDEBUG"; Config = "*-*-release" },
	},
}

local osx = {
	Inherit = common,
	Env = {
		CCOPTS = {
			"-Wall", "-Werror", "-g",
			{ "-O2"; Config = "*-*-release" },
		},
	},
}

local win = {
	Inherit = common,
	Env = {
		CCOPTS = {
			"/W4", "/wd4127", "/wd4100",
			{ "/O2"; Config = "*-*-release" },
		},
	},
}

local amiga = {
	Inherit = common,
	Env = {
		CPPDEFS = { "__AMIGA__", "NO_C_LIB" },
		CCOPTS = {
			"-warn=-1",
			"-dontwarn=163", "-dontwarn=307", "-dontwarn=65",
			"-dontwarn=166", "-dontwarn=167", "-dontwarn=81"
		},
	},
	ReplaceEnv = {
		["CC"] = "$(VBCC_ROOT)$(SEP)bin$(SEP)vc$(HOSTPROGSUFFIX)",
		["LIB"] = "$(VBCC_ROOT)$(SEP)bin$(SEP)vlink$(HOSTPROGSUFFIX)",
		["LD"] = "$(VBCC_ROOT)$(SEP)bin$(SEP)vc$(HOSTPROGSUFFIX)",
		["ASM"] = "$(VBCC_ROOT)$(SEP)bin$(SEP)vasmm68k_mot$(HOSTPROGSUFFIX)",
	},
}

Build {
	Passes = {
		Codegen = { Name = "Generate Code", BuildOrder = 1 },
	},
	Configs = {
		Config { Name = "linux-gcc", DefaultOnHost = "linux", Tools = { "gcc" }, Inherit = common },
		Config { Name = "macosx-gcc", DefaultOnHost = "macosx", Tools = { "gcc-osx" }, Inherit = osx },
		Config { Name = "win64-msvc", DefaultOnHost = "windows", Tools = { { "msvc-vs2015"; TargetPlatform = "x64" }, }, Inherit = win, },
		Config { Name = "amiga-vbcc", Inherit = amiga, Tools = { "vbcc" }, SupportedHosts = { "macosx", "windows", "linux" } },
	},
	Units = "units.lua",
	ScriptDirs = { "." },
}
