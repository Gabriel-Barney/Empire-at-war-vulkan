-- ==== Q6 t15 probe: object-script service (tools/probe_mod/q6_t15_probes/q6_objscript_hook.lua) ====
-- Appended by build_q6_mod.py to a copy of a shipped object script (Data\Scripts\GameObject\<name>.lua) with
-- @@SCRIPT@@ = the script name and @@RATE@@ = the ServiceRate this probe gives it ("nil" keeps the shipped rate).
-- Question: docs/re/OBJECT_SCRIPTS.md sec. 3.2 step 7 / sec. 8 item 1: the first service of an object script writes
-- LastService = -(synced draw in [0, hi]), hi probably ServiceRate; and do vanilla objects with Lua_Script run their
-- scripts at all (the port turns object scripts on only for mods until this is checked).
-- Records (per object-script instance = Lua state, sid = its own table address):
--   ATTACH  inside Base_Definitions (Definitions wrapper): rate after the shipped Definitions and after the override
--   MAIN    first line of main (the first pump resumes main)
--   PUMP    Pump_Service (called once per PumpEvents of the main thread, i.e. once per pump), first 6 pumps:
--           ls = LastService as seen during that pump (pump 1: absent; pump 2: the value written after pump 1 =
--           -(draw); pump 3: the frame time of pump 2), obj = the object's id, f = mode frame
-- f = round(GetCurrentTime() * 30); t = GetCurrentTime() with 6 decimals. Everything is pcall'd; the shipped script's
-- own behaviour is unchanged apart from ServiceRate when @@RATE@@ is a number.
Q6O_SCRIPT = "@@SCRIPT@@"
Q6O_RATE = @@RATE@@
Q6O_LOG = {}
Q6O_N = 0
Q6O_PUMPS = 0
Q6O_SID = string.sub((string.gsub(tostring(Q6O_LOG), "[^%w]", "")), -6)

function Q6O_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function Q6O_Num(v)
	if type(v) == "number" then
		return string.format("%.6f", v)
	end
	return Q6O_Str(v)
end

function Q6O_ObjId()
	local id = "?"
	pcall(function()
		if Object then
			id = tostring(Object.Get_ID())
		end
	end)
	if id == "?" then
		pcall(function() id = tostring(Object.Get_Object_ID()) end)
	end
	return Q6O_Str(id)
end

function Q6O_Rec(kind, fields)
	pcall(function()
		Q6O_N = Q6O_N + 1
		local fs, ts = "?", "?"
		local ok, t = pcall(GetCurrentTime)
		if ok and type(t) == "number" then
			fs = string.format("%.0f", t * 30)
			ts = string.format("%.6f", t)
		end
		Q6O_LOG[Q6O_N] = "Q6P|" .. kind .. "|p=obj|script=" .. Q6O_SCRIPT .. "|sid=" .. Q6O_SID .. "|n=" .. Q6O_N ..
			"|f=" .. fs .. "|t=" .. ts .. "|" .. (fields or "") .. "|END"
	end)
end

Q6O_Orig_Definitions = Definitions
function Definitions()
	if Q6O_Orig_Definitions then
		Q6O_Orig_Definitions()
	end
	local shipped = ServiceRate
	if type(Q6O_RATE) == "number" then
		ServiceRate = Q6O_RATE
	end
	Q6O_PUMPS = 0
	Q6O_Rec("ATTACH", "shipped_rate=" .. Q6O_Num(shipped) .. "|rate=" .. Q6O_Num(ServiceRate) .. "|ls=" ..
		Q6O_Num(LastService) .. "|pool=" .. Q6O_Str(ScriptPoolCount))
end

Q6O_Orig_main = main
function main()
	Q6O_Rec("MAIN", "obj=" .. Q6O_ObjId() .. "|ls=" .. Q6O_Num(LastService) .. "|rate=" .. Q6O_Num(ServiceRate))
	return Q6O_Orig_main()
end

Q6O_Orig_Pump_Service = Pump_Service
function Pump_Service()
	Q6O_PUMPS = Q6O_PUMPS + 1
	if Q6O_PUMPS <= 6 then
		local mode = "?"
		pcall(function() mode = Q6O_Str(Get_Game_Mode()) end)
		Q6O_Rec("PUMP", "k=" .. Q6O_PUMPS .. "|obj=" .. Q6O_ObjId() .. "|ls=" .. Q6O_Num(LastService) .. "|rate=" ..
			Q6O_Num(ServiceRate) .. "|mode=" .. mode)
	end
	if Q6O_Orig_Pump_Service then
		return Q6O_Orig_Pump_Service()
	end
end
-- ==== end of the Q6 t15 object-script probe ====
