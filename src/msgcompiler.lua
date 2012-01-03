module(..., package.seeall)

local nodegen = require "tundra.nodegen"

local mt = nodegen.create_eval_subclass {}

local blueprint = {
	Input = { Type = "string", Required = true },
	OutputStem = { Type = "string", Required = true },
}

function mt:create_dag(env, args, deps)
	local input = args.Input
	local stem = args.OutputStem
	local fnstem = "$(OBJECTDIR)/_generated/" .. stem
	local header = fnstem .. ".h"
	local source = fnstem .. ".c"
	return env:make_node {
		Label = "CompileNetMessages $(@)",
		Action = "python src/mkmsg.py " .. fnstem .. " < $(<)",
		Pass = args.Pass,
		InputFiles = { input },
		OutputFiles = { header, source },
		ImplicitInputs = { "src/mkmsg.py" },
		Dependencies = deps,
	}
end

nodegen.add_evaluator("CompileNetMessages", mt, blueprint)
