-- ==== a2f t07 budget probe: story plot script (channel S) ====
-- Plot-level Lua_Script of Story_Plots_Sandbox_12U_{Rebel,Underworld,Empire}.xml (mod override): it runs in the Heart
-- of the Maw campaign for whichever faction the human plays. Play REBEL or UNDERWORLD: then the Empire is an AI player
-- with a goal system and a budget; with the human as Empire there is no Empire budget and only Rebel / Underworld
-- are sampled.
--
-- What it records, per AI player (Empire, Rebel, Underworld), with the generated header + a2fb_core.lua in front:
--   * from the first service on: every service (ServiceRate 0.01 = every frame) for the first 4 s of galactic time,
--     then every frame in a window around each of the first four fiscal-cycle payments (45 s, 90 s, 135 s, 180 s:
--     44.5 s before to 2.5 s after), a small sample (SM: Sigma-of-pools CreditsUnnormalized, NetIncome, probe-type
--     BudgetFractionToBuild for ALL / Infrastructure / Offensive);
--   * a full sample (SM + BF + SH: all categories, the shipped weight / trigger equations) once per second until
--     120 s, every 5 s until 300 s (DONE); after that a full sample every 30 s for as long as the run lasts
--     (a2g t01: long runs until a template switch / Tech_Upgrade);
--   * a2h t02: an MJ record (MajorItem flush / reclaim triggers and inputs) with every Empire sample, and the message
--     TEXT_A2FB_LONG_DONE (record LONGDONE) at 1500 s = 25 galactic minutes, the length the MajorItem question needs;
--   * a2j t04: the message TEXT_A2FB_MIN30 (record MIN30) at 1800 s = 30 galactic minutes, the end of the a2j run.
--   * a2k t05 (only in a --saboteur build): the message TEXT_A2FB_SAB_DONE (record SABDONE) from frame
--     A2FB_SABCFG.stop2 + 10, the end of the saboteur run (frame 1400 + 10; a2m t12: 54000 + 10 with --sab-long).
-- The AI's budget is initialised at the first service of its galactic context (AI_GALACTIC 1.1: template init, then
-- budget init 0x6137b0), which can be after this script's first service: the first samples can therefore show the
-- state before Init (zero pools: BudgetFractionToBuild 1e18) and the following ones the state after it.
require("PGStoryMode")

A2FB_CANARY = "Mods\\A2fBudgetProbe\\A2FB_Canary_Story.txt"
A2FB_FACS = { "Empire", "Rebel", "Underworld", "Pirates" }
-- a2h t02: room for the MJ records and a long run (a2g's 20-minute run used 4870 of the core's default 8000)
A2FB_MAX = 20000
A2FB_LONG_T = 1500
A2FB_LONG_DONE = false
A2FB_MIN30_T = 1800
A2FB_MIN30_DONE = false
A2FB_LAST_FULL = -100
A2FB_STOPPED = false
A2FB_FIRST = true

-- a2i t02: every record below is built inside pcall (A2FB_Rec) and every player's sample runs in its own pcall, so a
-- Lua error costs one record or one sample and leaves an ERR record (each distinct message once), never the channel.
-- (Run 20261106-040725 lost this channel to an equation that fails inside the engine, which pcall cannot catch: see
-- a2fb_core.lua and the README's known limits.)
A2FB_Rec("LOAD", function() return "story plot script chunk ran modeq=" .. tostring(A2FB_MODEQ) end)
A2FB_Canary(A2FB_CANARY)

function Definitions()
	A2FB_Rec("DEF", function() return "Definitions ran" end)
	ServiceRate = 0.01
	StoryModeEvents = {}
	Define_State("A2FB_State", A2FB_State)
end

-- Seconds until the next fiscal window (44.5 s before each of the first four payments) starts.
function A2FB_NextWindow(t)
	for k = 1, 4 do
		local start = 45 * k - 0.5
		if t < start then
			return start - t
		end
	end
	return 1000
end

-- Which sample the time t asks for: returns dense (every service), full (a full sample is due), rate (next ServiceRate).
function A2FB_Plan(t)
	local dense = false
	if t < 4 then
		dense = true
	elseif t > 40 and t < 185 then
		local k = tonumber(string.format("%d", t / 45))
		local frac = t - k * 45
		if frac >= 44.45 or (frac < 2.5 and k >= 1) then
			dense = true
		end
	end
	local gap = 1
	if t >= 120 then
		gap = 5
	end
	local full = (t - A2FB_LAST_FULL) >= gap - 0.001
	local rate = gap
	if dense then
		rate = 0.01
	else
		-- be serviced again before the next fiscal window opens (the engine tests now - last > rate)
		local w = A2FB_NextWindow(t) - 0.05
		if w < rate then
			rate = w
		end
		if rate < 0.01 then
			rate = 0.01
		end
	end
	return dense, full, rate
end

function A2FB_Round(t, full)
	for i = 1, table.getn(A2FB_FACS) do
		local ok, p = pcall(Find_Player, A2FB_FACS[i])
		if ok and p ~= nil then
			local okh, human = pcall(function() return p.Is_Human() end)
			local is_human = okh and human
			-- full rounds cover every player (credits of the human and Pirates too), small rounds the AI players
			if full or (i <= 3 and not is_human) then
				local oks, errs = pcall(A2FB_Sample, "ST", p, full)
				if not oks then
					A2FB_Error("Sample " .. A2FB_FACS[i], errs)
				end
			end
		end
	end
end

function A2FB_Update()
	local t = A2FB_Now()
	if t < 0 then
		return
	end
	if A2FB_FIRST then
		A2FB_FIRST = false
		A2FB_Rec("FIRST", function()
			return "t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " humanexists=" .. tostring(A2FB_HumanExists())
		end)
		pcall(Game_Message, "TEXT_A2FB_LOADED")
	end
	-- a2k t05 saboteur run (build --saboteur: A2FB_SAB / A2FB_SABCFG in the header): the short run ends at its stop frame
	if A2FB_SAB and not A2FB_SAB_MSG and A2FB_Frame(t) >= A2FB_SABCFG.stop2 + 10 then
		A2FB_SAB_MSG = true
		A2FB_Rec("SABDONE", function() return "t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " records=" .. A2FB_Seq end)
		pcall(Game_Message, "TEXT_A2FB_SAB_DONE")
	end
	if t >= 300 then
		if not A2FB_STOPPED then
			A2FB_STOPPED = true
			A2FB_Rec("DONE", function() return "t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " records=" .. A2FB_Seq end)
			pcall(Game_Message, "TEXT_A2FB_DONE")
		end
		if t >= A2FB_LONG_T and not A2FB_LONG_DONE then
			A2FB_LONG_DONE = true
			A2FB_Rec("LONGDONE", function()
				return "t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " records=" .. A2FB_Seq
			end)
			pcall(Game_Message, "TEXT_A2FB_LONG_DONE")
		end
		-- a2j t04: the 30-minute run (D3 / D4 and the per-cycle P / I comparison) ends here
		if t >= A2FB_MIN30_T and not A2FB_MIN30_DONE then
			A2FB_MIN30_DONE = true
			A2FB_Rec("MIN30", function()
				return "t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " records=" .. A2FB_Seq
			end)
			pcall(Game_Message, "TEXT_A2FB_MIN30")
		end
		-- a2g t01 (long run until a template switch): keep a full sample every 30 s after DONE
		if (t - A2FB_LAST_FULL) >= 29.999 then
			A2FB_LAST_FULL = t
			A2FB_Round(t, true)
		end
		ServiceRate = 30
		return
	end
	local dense, full, rate = A2FB_Plan(t)
	if full then
		A2FB_LAST_FULL = t
	end
	A2FB_Round(t, full)
	ServiceRate = rate
end

function A2FB_State(message)
	if message == OnEnter then
		A2FB_Rec("ENTER", function() return "t=" .. A2FB_Num(A2FB_Now()) end)
	elseif message == OnUpdate then
		local ok, err = pcall(A2FB_Update)
		if not ok then
			A2FB_Error("Update", err)
		end
	end
end
