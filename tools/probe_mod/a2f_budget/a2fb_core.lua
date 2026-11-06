-- ==== a2f t07 budget probe: shared core (tools/probe_mod/a2f_budget/a2fb_core.lua) ====
-- Prepended (after the generated header: A2FB_CH, A2FB_MODEQ, A2FB_SPEC, A2FB_COMMON) to every probe script: the
-- story plot script, the galactic free store and the hooked plan scripts. Lua 5.0, and the free store has NO
-- math library (docs/re, s78g), so nothing here calls math.*, '#', '%' or string.rep.
--
-- Read-only: no GameRandom, no orders, no spawns, no story events. Every engine call is inside pcall; an evaluation
-- that fails inside the engine (EvaluatePerception raises the script-error helper and returns no value, which
-- the Lua sees as nil, AI_GALACTIC_3 sec. 8) is recorded as nil. But the helper also sets the script's failed flag,
-- and retail then stops servicing the script (a2i t02: run 20261106-040725 lost the story channel to one such
-- equation), so the probe evaluates only equations that answer in retail.
--
-- Output (docs/re/LUA_SCRIPT_OUTPUT.md): retail 1.121 writes no file, so every line is kept as a Lua string
--   A2FB + 1 + |<channel S, F or P>|<sid>|<seq>|<KIND>|<text>|END
-- in A2FB_Records; a2f_run.py finds them by a read-only scan of the game's memory. The needle is built by
-- concatenation so the script source never contains it. _CustomScriptMessage is called once on the channel's canary
-- file (DeleteFileA on first use): a missing canary after the run proves the script loaded.

A2FB_NEEDLE = "A2FB" .. "1|"
if A2FB_MAX == nil then
	A2FB_MAX = 8000
end

if A2FB_Records == nil then
	A2FB_Records = {}
	A2FB_Seq = 0
	A2FB_Sid = string.gsub(tostring(A2FB_Records), "^table: ", "")
	A2FB_Errors = {}
end

-- One record. Text is printable ASCII without '|' and at most 320 bytes (one scan hit is 400 bytes).
function A2FB_Say(kind, text)
	if A2FB_Seq >= A2FB_MAX then
		return
	end
	A2FB_Seq = A2FB_Seq + 1
	local s = string.gsub(tostring(text), "[^ -~]", "?")
	s = string.gsub(s, "|", "/")
	s = string.sub(s, 1, 320)
	A2FB_Records[A2FB_Seq] = A2FB_NEEDLE .. A2FB_CH .. "|" .. A2FB_Sid .. "|" .. A2FB_Seq .. "|" .. kind .. "|" .. s .. "|END"
end

-- Each distinct error once.
function A2FB_Error(where, err)
	local key = where .. " " .. tostring(err)
	if A2FB_Errors[key] == nil then
		A2FB_Errors[key] = true
		A2FB_Say("ERR", key)
	end
end

function A2FB_Num(v)
	if type(v) == "number" then
		return string.format("%.6g", v)
	end
	if v == nil then
		return "nil"
	end
	return "T:" .. type(v)
end

function A2FB_Str(f)
	local ok, v = pcall(f)
	if not ok then
		return "ERR"
	end
	if type(v) == "number" then
		return A2FB_Num(v)
	end
	if v == nil then
		return "nil"
	end
	return tostring(v)
end

function A2FB_Now()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return -1
end

-- Mode frame = round(GetCurrentTime() * 30) (GetCurrentTime() = frame / 30), no math library.
function A2FB_Frame(t)
	if t < 0 then
		return -1
	end
	return tonumber(string.format("%d", t * 30 + 0.5))
end

function A2FB_Canary(path)
	if A2FB_CanaryDone then
		return
	end
	A2FB_CanaryDone = true
	local ok, err = pcall(_CustomScriptMessage, path, "a2fb canary\n")
	A2FB_Say("CANARY", "file=" .. path .. " ok=" .. tostring(ok) .. " err=" .. tostring(err))
end

function A2FB_Eval(name, player)
	local ok, v = pcall(EvaluatePerception, name, player)
	if not ok then
		return "ERR"
	end
	return A2FB_Num(v)
end

function A2FB_Fac(p)
	local ok, n = pcall(function() return p.Get_Faction_Name() end)
	if ok and type(n) == "string" then
		return string.upper(n)
	end
	return "?"
end

-- True when any of the four faction players is human (Variable_Human exists; some shipped equations need it).
function A2FB_HumanExists()
	local names = { "Empire", "Rebel", "Underworld", "Pirates" }
	for i = 1, 4 do
		local ok, p = pcall(Find_Player, names[i])
		if ok and p ~= nil then
			local okh, h = pcall(function() return p.Is_Human() end)
			if okh and h then
				return true
			end
		end
	end
	return false
end

function A2FB_Join(list, p)
	local s = ""
	for i = 1, table.getn(list) do
		s = s .. " " .. list[i][1] .. "=" .. A2FB_Eval(list[i][2], p)
	end
	return s
end

-- One sample of player p. Records (each carries tag, time, frame and faction):
--   PL  credits, tech level, human flag, difficulty               (full samples only)
--   SM  cr=credits +  common + small BFB group: CU, NET, B1/B2 for ALL, INFRA, OFF   (mod equations)
--   BF  the remaining BFB categories (full samples only)
--   SH  the shipped budget-weight / trigger equations (full samples only)
--   MJ  a2h t02: MajorItem flush / reclaim triggers and inputs (every sample; factions with spec.major: Empire)
-- key B<k><CAT> = A2FB_<FAC>_BFB_<k>_<CAT> = BudgetFractionToBuild of probe type k in category CAT (CAT ALL = no
-- Parameter_Category). Decoded by a2f_report.py.
-- a2i t02: each record is built inside pcall (A2FB_Rec), so a Lua error in one record (a binding, a nil in the text)
-- costs that record only and leaves one ERR record "<KIND> <message>" (each distinct message once). pcall does NOT
-- catch an equation that fails inside the engine: that sets the script's failed flag without a Lua error and the
-- script stops (run 20261106-040725, A2FB_ActiveGoals); only equations that answer in retail may be evaluated.
function A2FB_Rec(kind, f)
	local ok, s = pcall(f)
	if ok then
		A2FB_Say(kind, s)
	else
		A2FB_Error(kind, s)
	end
end

-- Text of the SH / MJ records: list entries { key, equation, needs a human }; skip without a human player.
function A2FB_JoinH(head, list, p)
	local s = head
	local hx = A2FB_HumanExists()
	for i = 1, table.getn(list) do
		local e = list[i]
		if e[3] and not hx then
			s = s .. " " .. e[1] .. "=skip"
		else
			s = s .. " " .. e[1] .. "=" .. A2FB_Eval(e[2], p)
		end
	end
	return s
end

function A2FB_Sample(tag, p, full)
	local t = A2FB_Now()
	local fac = A2FB_Fac(p)
	local head = "tag=" .. tag .. " t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " fac=" .. fac
	local human = A2FB_Str(function() return p.Is_Human() end)
	local cr = A2FB_Str(function() return p.Get_Credits() end)
	if full then
		A2FB_Rec("PL", function()
			return head .. " human=" .. human .. " cr=" .. cr ..
				" tech=" .. A2FB_Str(function() return p.Get_Tech_Level() end) ..
				" diff=" .. A2FB_Str(function() return p.Get_Difficulty() end)
		end)
	end
	head = head .. " cr=" .. cr
	local spec = A2FB_SPEC[fac]
	if spec == nil or human == "true" then
		return
	end
	if A2FB_MODEQ then
		A2FB_Rec("SM", function() return head .. A2FB_Join(A2FB_COMMON, p) .. A2FB_Join(spec.small, p) end)
		if full then
			A2FB_Rec("BF", function() return head .. A2FB_Join(spec.full, p) end)
		end
	end
	if full then
		A2FB_Rec("SH", function() return A2FB_JoinH(head, spec.shipped, p) end)
	end
	-- a2h t02: MajorItem flush / reclaim triggers and inputs, every sample (small and full) of the factions that have
	-- spec.major (Empire). A mod equation entry exists only when the mod equation file is in use.
	if spec.major ~= nil then
		A2FB_Rec("MJ", function() return A2FB_JoinH(head, spec.major, p) end)
	end
end
-- ==== end of the shared core ====
