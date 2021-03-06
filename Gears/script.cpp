#define NOMINMAX
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include "../../ScriptHookV_SDK/inc/natives.h"
#include "../../ScriptHookV_SDK/inc/enums.h"
#include "../../ScriptHookV_SDK/inc/main.h"
#include "../../ScriptHookV_SDK/inc/types.h"

#include "script.h"

#include "ScriptSettings.hpp"
#include "VehicleData.hpp"

#include "Input/ScriptControls.hpp"
#include "Memory/MemoryPatcher.hpp"
#include "Util/Logger.hpp"
#include "Util/Util.hpp"
#include "Util/Paths.h"

#include "menu.h"
#include "Memory/NativeMemory.hpp"
#include "Util/MathExt.h"
#include "ShiftModes.h"
std::array<float, 8> upshiftSpeeds{};

std::string textureWheelFile;
int textureWheelId;

GameSound gearRattle("DAMAGED_TRUCK_IDLE", 0);
int soundID;
float stallingProgress = 0.0f;

std::string settingsGeneralFile;
std::string settingsWheelFile;
std::string settingsStickFile;
std::string settingsMenuFile;

NativeMenu::Menu menu;
ScriptControls controls;
ScriptSettings settings;

Player player;
Ped playerPed;
Vehicle vehicle;
Vehicle lastVehicle;
VehicleData vehData;
VehicleExtensions ext;

int prevNotification = 0;
int prevExtShift = 0;

int speedoIndex;
extern std::vector<std::string> speedoTypes;

bool lookrightfirst = false;

void initVehicle() {
	reset();
	std::fill(upshiftSpeeds.begin(), upshiftSpeeds.end(), 0.0f);
	vehData = VehicleData();
	vehData.UpdateValues(ext, vehicle);

	if (vehData.NoClutch) {
		vehData.SimulatedNeutral = false;
	}
	else {
		vehData.SimulatedNeutral = settings.DefaultNeutral;
	}
	shiftTo(1, true);
	initSteeringPatches();
}

void update() {
	///////////////////////////////////////////////////////////////////////////
	//                     Are we in a supported vehicle?
	///////////////////////////////////////////////////////////////////////////
	player = PLAYER::PLAYER_ID();
	playerPed = PLAYER::PLAYER_PED_ID();

	if (!ENTITY::DOES_ENTITY_EXIST(playerPed) ||
		!PLAYER::IS_PLAYER_CONTROL_ON(player) ||
		ENTITY::IS_ENTITY_DEAD(playerPed) ||
		PLAYER::IS_PLAYER_BEING_ARRESTED(player, TRUE)) {
		return;
	}

	vehicle = PED::GET_VEHICLE_PED_IS_IN(playerPed, false);

	if (!vehicle || !ENTITY::DOES_ENTITY_EXIST(vehicle) || 
		playerPed != VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle, -1)) {
		return;
	}

	///////////////////////////////////////////////////////////////////////////
	//                       Update vehicle and inputs
	///////////////////////////////////////////////////////////////////////////

	if (vehicle != lastVehicle) {
		initVehicle();
	}
	lastVehicle = vehicle;

	vehData.UpdateValues(ext, vehicle);
	bool ignoreClutch = false;
	if (settings.ShiftMode == Automatic ||
		vehData.Class == VehicleData::VehicleClass::Bike && settings.SimpleBike) {
		ignoreClutch = true;
	}

	updateLastInputDevice();
	controls.UpdateValues(controls.PrevInput, ignoreClutch, false);

	if (settings.CrossScript) {
		crossScriptComms();
		crossScriptUpdated();
	}

	///////////////////////////////////////////////////////////////////////////
	//                                   HUD
	///////////////////////////////////////////////////////////////////////////
	if (settings.DisplayInfo) {
		drawDebugInfo();
	}
	if (settings.DisplayWheelInfo) {
		drawVehicleWheelInfo();
	}
	if (settings.HUD &&
		(settings.EnableManual || settings.AlwaysHUD)) {
		drawHUD();
	}
	if (settings.HUD &&
		(controls.PrevInput == ScriptControls::Wheel || settings.AlwaysSteeringWheelInfo) &&
		settings.SteeringWheelInfo && textureWheelId != -1) {
		drawInputWheelInfo();
	}

	if (controls.ButtonJustPressed(ScriptControls::KeyboardControlType::Toggle) ||
		controls.ButtonJustPressed(ScriptControls::WheelControlType::Toggle) ||
		controls.ButtonHeld(ScriptControls::ControllerControlType::Toggle) ||
		controls.PrevInput == ScriptControls::Controller	&& controls.ButtonHeld(ScriptControls::LegacyControlType::Toggle)) {
		toggleManual();
		return;
	}

	///////////////////////////////////////////////////////////////////////////
	//                            Alt vehicle controls
	///////////////////////////////////////////////////////////////////////////

	if (settings.EnableWheel && controls.PrevInput == ScriptControls::Wheel) {

		if (vehData.Class == VehicleData::VehicleClass::Boat) {
			handleVehicleButtons();
			handlePedalsDefault(controls.ThrottleVal, controls.BrakeVal);
			doWheelSteering();
			playFFBGround(false, true);
			return;
		}

		if (vehData.Class == VehicleData::VehicleClass::Plane)
			return;

		if (vehData.Class == VehicleData::VehicleClass::Heli)
			return;
	}
	
	///////////////////////////////////////////////////////////////////////////
	//                          Ground vehicle controls
	///////////////////////////////////////////////////////////////////////////

	if (!settings.EnableManual &&
		settings.EnableWheel && settings.WheelWithoutManual &&
		controls.WheelControl.IsConnected(controls.SteerGUID)) {

		handleVehicleButtons();
		handlePedalsDefault(controls.ThrottleVal, controls.BrakeVal);
		doWheelSteering();
		bool airborne = !VEHICLE::IS_VEHICLE_ON_ALL_WHEELS(vehicle) &&
			ENTITY::GET_ENTITY_HEIGHT_ABOVE_GROUND(vehicle) > 1.25f;
		playFFBGround(airborne);
	}	

	///////////////////////////////////////////////////////////////////////////
	//          Active whenever Manual is enabled from here
	///////////////////////////////////////////////////////////////////////////
	if (!settings.EnableManual) {
		return;
	}

	if (MemoryPatcher::TotalPatched != MemoryPatcher::NumGearboxPatches) {
		MemoryPatcher::PatchInstructions();
	}
	
	handleVehicleButtons();

	if (settings.EnableWheel && controls.WheelControl.IsConnected(controls.SteerGUID)) {
		doWheelSteering();
		bool airborne = !VEHICLE::IS_VEHICLE_ON_ALL_WHEELS(vehicle) &&
			ENTITY::GET_ENTITY_HEIGHT_ABOVE_GROUND(vehicle) > 1.25f;
		playFFBGround(airborne);
	}
	

	if (controls.ButtonJustPressed(ScriptControls::KeyboardControlType::ToggleH) ||
		controls.ButtonJustPressed(ScriptControls::WheelControlType::ToggleH) ||
		controls.ButtonHeld(ScriptControls::ControllerControlType::ToggleH) ||
		controls.PrevInput == ScriptControls::Controller	&& controls.ButtonHeld(ScriptControls::LegacyControlType::ToggleH)) {
		cycleShiftMode();
	}

	///////////////////////////////////////////////////////////////////////////
	//                            Actual mod operations
	///////////////////////////////////////////////////////////////////////////

	// Reverse behavior
	// For bikes, do this automatically.
	if (vehData.Class == VehicleData::VehicleClass::Bike && settings.SimpleBike) {
		if (controls.PrevInput == ScriptControls::InputDevices::Wheel) {
			handlePedalsDefault( controls.ThrottleVal, controls.BrakeVal );
		} else {
			functionAutoReverse();
		}
	}
	else {
		if (controls.PrevInput == ScriptControls::InputDevices::Wheel) {
			handlePedalsRealReverse( controls.ThrottleVal, controls.BrakeVal );
		} else {
			functionRealReverse();
		}
	}

	// Limit truck speed per gear upon game wanting to shift, but we block it.
	if (vehData.IsTruck) {
		functionTruckLimiting();
	}
	
	if (settings.EngBrake) {
		functionEngBrake();
	}

	// Engine damage: RPM Damage
	if (settings.EngDamage &&
		!vehData.NoClutch) {
		functionEngDamage();
	}

	if (settings.EngLock) {
		functionEngLock();
	}

	if (!vehData.SimulatedNeutral && 
		!(settings.SimpleBike && vehData.Class == VehicleData::VehicleClass::Bike) && 
		!vehData.NoClutch) {
		// Stalling
		if (settings.EngStall && settings.ShiftMode == HPattern ||
			settings.EngStallS && settings.ShiftMode == Sequential) {
			functionEngStall();
		}

		// Simulate "catch point"
		// When the clutch "grabs" and the car starts moving without input
		if (settings.ClutchCatching) {
			functionClutchCatch();
		}
	}

	// Manual shifting
	switch(settings.ShiftMode) {
		case Sequential: {
			functionSShift();
			if (settings.AutoGear1) {
				functionAutoGear1();
			}
			break;
		}
		case HPattern: {
			if (controls.PrevInput == ScriptControls::Wheel) {
				functionHShiftWheel();
			}
			if (controls.PrevInput == ScriptControls::Keyboard ||
				controls.PrevInput == ScriptControls::Wheel && settings.HPatternKeyboard) {
				functionHShiftKeyboard();
			}
			break;
		}
		case Automatic: {
			functionAShift();
			break;
		}
		default: break;
	}

	if (settings.AutoLookBack) {
		functionAutoLookback();
	}

	if (settings.HillBrakeWorkaround) {
		functionHillGravity();
	}

	if (gearRattle.Active) {
		if (controls.ClutchVal > 1.0f - settings.ClutchCatchpoint ||
			!VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
			gearRattle.Stop();
		}
	}

	// Finally, update memory each loop
	handleRPM();
	ext.SetGearCurr(vehicle, vehData.LockGear);
	ext.SetGearNext(vehicle, vehData.LockGear);
}

///////////////////////////////////////////////////////////////////////////////
//                           Display elements
///////////////////////////////////////////////////////////////////////////////
void drawRPMIndicator(float x, float y, float width, float height, Color fg, Color bg, float rpm) {
	float bgpaddingx = 0.00f;
	float bgpaddingy = 0.01f;
	// background
	GRAPHICS::DRAW_RECT(x, y, width+bgpaddingx, height+bgpaddingy, bg.R, bg.G, bg.B, bg.A);

	// rpm thingy
	GRAPHICS::DRAW_RECT(x-width*0.5f+rpm*width*0.5f, y, width*rpm, height, fg.R, fg.G, fg.B, fg.A);
}

void drawRPMIndicator() {
	Color background = {
		settings.RPMIndicatorBackgroundR,
		settings.RPMIndicatorBackgroundG,
		settings.RPMIndicatorBackgroundB,
		settings.RPMIndicatorBackgroundA
	};

	Color foreground = {
		settings.RPMIndicatorForegroundR,
		settings.RPMIndicatorForegroundG,
		settings.RPMIndicatorForegroundB,
		settings.RPMIndicatorForegroundA
	};

	Color rpmcolor = foreground;
	if (vehData.Rpm > settings.RPMIndicatorRedline) {
		Color redline = {
			settings.RPMIndicatorRedlineR,
			settings.RPMIndicatorRedlineG,
			settings.RPMIndicatorRedlineB,
			settings.RPMIndicatorRedlineA
		};
		rpmcolor = redline;
	}
	if (vehData.CurrGear < vehData.NextGear || vehData.TruckShiftUp) {
		Color rpmlimiter = {
			settings.RPMIndicatorRevlimitR,
			settings.RPMIndicatorRevlimitG,
			settings.RPMIndicatorRevlimitB,
			settings.RPMIndicatorRevlimitA
		};
		rpmcolor = rpmlimiter;
	}
	drawRPMIndicator(
		settings.RPMIndicatorXpos,
		settings.RPMIndicatorYpos,
		settings.RPMIndicatorWidth,
		settings.RPMIndicatorHeight,
		rpmcolor,
		background,
		vehData.Rpm
	);
}

std::string formatSpeedo(std::string units, float speed, bool showUnit, int hudFont) {
	std::stringstream speedoFormat;
	if (units == "kph") speed = speed * 3.6f;
	if (units == "mph") speed = speed / 0.44704f;

	speedoFormat << std::setfill('0') << std::setw(3) << std::to_string(static_cast<int>(std::round(speed)));
	if (hudFont != 2 && units == "kph") units = "km/h";
	if (hudFont != 2 && units == "ms") units = "m/s";
	if (showUnit) speedoFormat << " " << units;

	return speedoFormat.str();
}

void drawSpeedoMeter() {
	float dashms = ext.GetDashSpeed(vehicle);
	if (!vehData.HasSpeedo && dashms > 0.1f) {
		vehData.HasSpeedo = true;
	}
	if (!vehData.HasSpeedo) {
		dashms = abs(vehData.Velocity);
	}

	showText(settings.SpeedoXpos, settings.SpeedoYpos, settings.SpeedoSize, 
		formatSpeedo(settings.Speedo, dashms, settings.SpeedoShowUnit, settings.HUDFont),
		settings.HUDFont, solidWhite, true);
}

void drawShiftModeIndicator() {
	std::string shiftModeText;
	auto color = solidWhite;
	switch (settings.ShiftMode) {
		case Sequential: shiftModeText = "S";
			break;
		case HPattern: shiftModeText = "H";
			break;
		case Automatic: shiftModeText = "A";
			break;
		default: shiftModeText = "";
			break;
	}
	if (!settings.EnableManual) {
		shiftModeText = "A";
		color = { 0, 126, 232, 255 };
	}
	showText(settings.ShiftModeXpos, settings.ShiftModeYpos, settings.ShiftModeSize, shiftModeText, settings.HUDFont, color, true);
}

void drawGearIndicator() {
	std::string gear = std::to_string(vehData.CurrGear);
	if (vehData.SimulatedNeutral && settings.EnableManual) {
		gear = "N";
	}
	else if (vehData.CurrGear == 0) {
		gear = "R";
	}
	Color c;
	if (vehData.CurrGear == vehData.TopGear) {
		c.R = settings.GearTopColorR;
		c.G = settings.GearTopColorG;
		c.B = settings.GearTopColorB;
		c.A = 255;
	}
	else {
		c = solidWhite;
	}
	showText(settings.GearXpos, settings.GearYpos, settings.GearSize, gear, settings.HUDFont, c, true);
}

void drawHUD() {
	if (settings.GearIndicator) {
		drawGearIndicator();
	}
	if (settings.ShiftModeIndicator) {
		drawShiftModeIndicator();
	}
	if (settings.Speedo == "kph" ||
		settings.Speedo == "mph" ||
		settings.Speedo == "ms") {
		drawSpeedoMeter();
	}
	if (settings.RPMIndicator) {
		drawRPMIndicator();
	}
}

void drawDebugInfo() {
	std::stringstream ssEnabled;
	std::stringstream ssRPM;
	std::stringstream ssCurrGear;
	std::stringstream ssNextGear;
	std::stringstream ssClutch;
	std::stringstream ssThrottle;
	std::stringstream ssTurbo;
	std::stringstream ssAddress;
	std::stringstream ssDashSpd;
	std::stringstream ssDbias;

	ssEnabled << "Mod:\t\t" << (settings.EnableManual ? "Enabled" : "Disabled");
	ssRPM		<< "RPM:\t\t" << std::setprecision(3) << vehData.Rpm;
	ssCurrGear	<< "CurrGear:\t" << vehData.CurrGear;
	ssNextGear	<< "NextGear:\t" << vehData.NextGear;
	ssClutch	<< "Clutch:\t\t" << std::setprecision(3) << vehData.Clutch;
	ssThrottle	<< "Throttle:\t" << std::setprecision(3) << vehData.Throttle;
	ssTurbo		<< "Turbo:\t\t" << std::setprecision(3) << vehData.Turbo;
	ssAddress	<< "Address:\t" << std::hex << reinterpret_cast<uint64_t>(ext.GetAddress(vehicle));
	ssDashSpd	<< "Speedo:\t" << (vehData.HasSpeedo ? "Yes" : "No");
	ssDbias		<< "DBias:\t\t" << std::setprecision(3) << vehData.DriveBiasFront;

	showText(0.01f, 0.275f, 0.4f, ssEnabled.str(),	4, solidWhite, true);
	showText(0.01f, 0.300f, 0.4f, ssRPM.str(),		4, solidWhite, true);
	showText(0.01f, 0.325f, 0.4f, ssCurrGear.str(),	4, solidWhite, true);
	showText(0.01f, 0.350f, 0.4f, ssNextGear.str(),	4, solidWhite, true);
	showText(0.01f, 0.375f, 0.4f, ssClutch.str(),	4, solidWhite, true);
	showText(0.01f, 0.400f, 0.4f, ssThrottle.str(),	4, solidWhite, true);
	showText(0.01f, 0.425f, 0.4f, ssTurbo.str(),	4, solidWhite, true);
	showText(0.01f, 0.450f, 0.4f, ssAddress.str(),	4, solidWhite, true);
	showText(0.01f, 0.475f, 0.4f, ssDashSpd.str(),	4, solidWhite, true);
	showText(0.01f, 0.500f, 0.4f, ssDbias.str(),	4, solidWhite, true);

	std::stringstream ssThrottleInput;
	std::stringstream ssBrakeInput;
	std::stringstream ssClutchInput;
	std::stringstream ssHandbrakInput;

	ssThrottleInput << "Throttle:\t" << controls.ThrottleVal;
	ssBrakeInput	<< "Brake:\t\t" << controls.BrakeVal;
	ssClutchInput	<< "Clutch:\t\t" << controls.ClutchValRaw;
	ssHandbrakInput << "Handb:\t\t" << controls.HandbrakeVal;

	showText(0.85, 0.050, 0.4, ssThrottleInput.str(),	4, solidWhite, true);
	showText(0.85, 0.075, 0.4, ssBrakeInput.str(),		4, solidWhite, true);
	showText(0.85, 0.100, 0.4, ssClutchInput.str(),		4, solidWhite, true);
	showText(0.85, 0.125, 0.4, ssHandbrakInput.str(),	4, solidWhite, true);

	if (settings.EnableWheel) {
		std::stringstream dinputDisplay;
		dinputDisplay << "Wheel" << (controls.WheelControl.IsConnected(controls.SteerGUID) ? "" : " not") << " present";
		showText(0.85, 0.150, 0.4, dinputDisplay.str(), 4, solidWhite, true);
	}

	if (settings.EnableManual && settings.DisplayGearingInfo) {
		auto ratios = ext.GetGearRatios(vehicle);
		float DriveMaxFlatVel = ext.GetDriveMaxFlatVel(vehicle);
		int i = 0;
		showText(0.25f, 0.05f, 0.5f, "Expected");
		for (auto ratio : ratios) {
			float maxSpeed = DriveMaxFlatVel / ratio;
			showText(0.25f, 0.10f + 0.025f * i, 0.35f, "G" + std::to_string(i) + ": " + std::to_string(maxSpeed));
			i++;
		}

		i = 0;
		showText(0.40f, 0.05f, 0.5f, "Actual");
		for (auto speed : upshiftSpeeds) {
			showText(0.40f, 0.10f + 0.025f * i, 0.35f, "G" + std::to_string(i) + ": " + std::to_string(speed));
			i++;
		}
	}
}

void showDebugInfoWheel(ScriptSettings &settings, float effSteer, int damperForce, float steerSpeed, double GForce, float oversteer, float understeer) {
	std::stringstream SteerRaw;
	std::stringstream SteerNorm;
	std::stringstream steerDisplay;
	std::stringstream GForceDisplay;
	std::stringstream damperF;
	std::stringstream ssUnderSteer;
	std::stringstream ssOverSteer;
	
	SteerRaw		<< "SteerRaw:\t" << controls.SteerVal;
	SteerNorm		<< "SteerNorm:\t" << effSteer;
	steerDisplay	<< "SteerSpeed:\t" << steerSpeed;
	GForceDisplay	<< "GForceFinal:\t" <<
		std::setprecision(5) << (-GForce * 5000 * settings.PhysicsStrength * settings.FFGlobalMult);
	damperF			<< "DampForce:\t" << steerSpeed * damperForce * 0.1;
	ssUnderSteer	<< "Understeer:\t" << understeer;
	ssOverSteer		<< "Oversteer:\t" << oversteer;

	showText(0.85, 0.175, 0.4, SteerRaw.str(),		4, solidWhite, true);
	showText(0.85, 0.200, 0.4, SteerNorm.str(),		4, solidWhite, true);
	showText(0.85, 0.225, 0.4, steerDisplay.str(),	4, solidWhite, true);
	showText(0.85, 0.250, 0.4, GForceDisplay.str(),	4, solidWhite, true);
	showText(0.85, 0.275, 0.4, damperF.str(),		4, solidWhite, true);
	showText(0.85, 0.300, 0.4, ssUnderSteer.str(),	4, solidWhite, true);
	showText(0.85, 0.325, 0.4, ssOverSteer.str(),	4, solidWhite, true);
}

void drawInputWheelInfo() {
	// Steering Wheel
	float rotation = settings.SteerAngleMax * (controls.SteerVal - 0.5f);
	if (controls.PrevInput != ScriptControls::Wheel) rotation = 90.0f * -ext.GetSteeringInputAngle(vehicle);

	drawTexture(textureWheelId, 0, -9998, 100, 
				settings.SteeringWheelTextureSz, settings.SteeringWheelTextureSz, 
				0.5f, 0.5f, // center of texture
				settings.SteeringWheelTextureX, settings.SteeringWheelTextureY,
				rotation/360.0f, GRAPHICS::_GET_ASPECT_RATIO(FALSE), 1.0f, 1.0f, 1.0f, 1.0f);

	// Pedals
	float barWidth = settings.PedalInfoW/3.0f;

	float barYBase = (settings.PedalInfoY + settings.PedalInfoH * 0.5f);

	GRAPHICS::DRAW_RECT(settings.PedalInfoX , settings.PedalInfoY, 3.0f * barWidth + settings.PedalInfoPadX, settings.PedalInfoH + settings.PedalInfoPadY, 0, 0, 0, 92);
	GRAPHICS::DRAW_RECT(settings.PedalInfoX - 1.0f*barWidth, barYBase - controls.ThrottleVal*settings.PedalInfoH*0.5f, barWidth, controls.ThrottleVal*settings.PedalInfoH, 0, 255, 0, 255);
	GRAPHICS::DRAW_RECT(settings.PedalInfoX + 0.0f*barWidth, barYBase - controls.BrakeVal*settings.PedalInfoH*0.5f, barWidth, controls.BrakeVal*settings.PedalInfoH, 255, 0, 0, 255);
	GRAPHICS::DRAW_RECT(settings.PedalInfoX + 1.0f*barWidth, barYBase - controls.ClutchValRaw*settings.PedalInfoH*0.5f, barWidth, controls.ClutchVal*settings.PedalInfoH, 0, 0, 255, 255);
}

///////////////////////////////////////////////////////////////////////////////
//                            Helper things
///////////////////////////////////////////////////////////////////////////////
void crossScriptUpdated() {
	// Current gear
	DECORATOR::DECOR_SET_INT(vehicle, "mt_gear", vehData.CurrGear);

	// Shift indicator: 0 = nothing, 1 = Shift up, 2 = Shift down
	if (vehData.CurrGear < vehData.NextGear || vehData.TruckShiftUp) {
		DECORATOR::DECOR_SET_INT(vehicle, "mt_shift_indicator", 1);
	}
	else if (vehData.CurrGear > 1 && vehData.Rpm < 0.4f) {
		DECORATOR::DECOR_SET_INT(vehicle, "mt_shift_indicator", 2);
	}
	else if (vehData.CurrGear == vehData.NextGear) {
		DECORATOR::DECOR_SET_INT(vehicle, "mt_shift_indicator", 0);
	}

	// Simulated Neutral
	if (vehData.SimulatedNeutral && settings.EnableManual) {
		DECORATOR::DECOR_SET_INT(vehicle, "mt_neutral", 1);
	}
	else {
		DECORATOR::DECOR_SET_INT(vehicle, "mt_neutral", 0);
	}

	// External shifting
	int currExtShift = DECORATOR::DECOR_GET_INT(vehicle, "mt_set_shiftmode");
	if (prevExtShift != currExtShift && currExtShift > 0) {
		// 1 Seq, 2 H, 3 Auto
		setShiftMode(currExtShift - 1);
	}
	prevExtShift = currExtShift;

	DECORATOR::DECOR_SET_INT(vehicle, "mt_get_shiftmode", static_cast<int>(settings.ShiftMode) + 1);
}

// TODO: Phase out
void crossScriptComms() {
	// Current gear
	DECORATOR::DECOR_SET_INT(vehicle, "doe_elk", vehData.CurrGear);

	// Shift indicator: 0 = nothing, 1 = Shift up, 2 = Shift down
	if (vehData.CurrGear < vehData.NextGear || vehData.TruckShiftUp) {
		DECORATOR::DECOR_SET_INT(vehicle, "hunt_score", 1);
	}
	else if (vehData.CurrGear > 1 && vehData.Rpm < 0.4f) {
		DECORATOR::DECOR_SET_INT(vehicle, "hunt_score", 2);
	}
	else if (vehData.CurrGear == vehData.NextGear) {
		DECORATOR::DECOR_SET_INT(vehicle, "hunt_score", 0);
	}

	// Simulated Neutral
	if (vehData.SimulatedNeutral && settings.EnableManual) {
		DECORATOR::DECOR_SET_INT(vehicle, "hunt_weapon", 1);
	}
	else {
		DECORATOR::DECOR_SET_INT(vehicle, "hunt_weapon", 0);
	}

	// External shifting
	int currExtShift = DECORATOR::DECOR_GET_INT(vehicle, "hunt_chal_weapon");
	if (prevExtShift != currExtShift && currExtShift > 0) {
		// 1 Seq, 2 H, 3 Auto
		setShiftMode(currExtShift - 1);
	}
	prevExtShift = currExtShift;
}

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Mod control
///////////////////////////////////////////////////////////////////////////////

void initialize() {
	settings.Read(&controls);
	menu.ReadSettings();
	logger.Write("Settings read");

	speedoIndex = static_cast<int>(std::find(speedoTypes.begin(), speedoTypes.end(), settings.Speedo) - speedoTypes.begin());
	if (speedoIndex >= speedoTypes.size()) {
		speedoIndex = 0;
	}

	vehData.LockGear = 1;
	vehData.SimulatedNeutral = settings.DefaultNeutral;
	initWheel();
}

void reset() {
	resetSteeringMultiplier();
	gearRattle.Stop();
	if (MemoryPatcher::TotalPatched == MemoryPatcher::NumGearboxPatches) {
		MemoryPatcher::RestoreInstructions();
	}
	if (MemoryPatcher::SteeringPatched) {
		MemoryPatcher::RestoreSteeringCorrection();
	}
	if (MemoryPatcher::BrakeDecrementPatched) {
		MemoryPatcher::RestoreBrakeDecrement();
	}
	controls.StopForceFeedback();
	//lastVehicle = vehicle = 0;
}

void toggleManual() {
	settings.EnableManual = !settings.EnableManual;
	settings.SaveGeneral();
	std::stringstream message;
	message << "Manual Transmission " <<
	           (settings.EnableManual ? "Enabled" : "Disabled");
	showNotification(message.str(), &prevNotification);
	logger.Write(message.str());
	if (ENTITY::DOES_ENTITY_EXIST(vehicle)) {
		VEHICLE::SET_VEHICLE_HANDBRAKE(vehicle, false);
		VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, true, false, true);
	}
	if (!settings.EnableManual) {
		reset();
	}
	initialize();
	initSteeringPatches();
}

void initWheel() {
	if (settings.EnableWheel) {
		controls.InitWheel(settings.EnableFFB);
//		controls.StickControl.InitDevice();
		controls.CheckGUIDs(settings.reggdGuids);
	}
	controls.SteerGUID = controls.WheelAxesGUIDs[static_cast<int>(controls.SteerAxisType)];
}

void initSteeringPatches() {
	if (controls.PrevInput != ScriptControls::Wheel && settings.LogiLEDs) {
		for (GUID guid : controls.WheelControl.GetGuids()) {
			controls.WheelControl.PlayLedsDInput(guid, 0.0, 0.5, 1.0);
		}
	}
	if (controls.PrevInput == ScriptControls::Wheel || settings.PatchSteeringAlways) {
		if (!MemoryPatcher::SteeringPatched && settings.PatchSteering) {
			MemoryPatcher::PatchSteeringCorrection();
		}
		applySteeringMultiplier();
	}
	else {
		if (MemoryPatcher::SteeringPatched && settings.PatchSteering && !settings.PatchSteeringAlways) {
			MemoryPatcher::RestoreSteeringCorrection();
		}
		resetSteeringMultiplier();
	}
	if (controls.PrevInput == ScriptControls::Wheel && vehData.Class == VehicleData::VehicleClass::Car && settings.PatchSteeringControl) {
		if (!MemoryPatcher::SteerControlPatched) {
			MemoryPatcher::PatchSteeringControl();
		}
	}
	else {
		if (MemoryPatcher::SteerControlPatched) {
			MemoryPatcher::RestoreSteeringControl();
		}
	}
}

void applySteeringMultiplier() {
	if (vehicle != 0) {
		ext.SetSteeringMultiplier(vehicle, settings.GameSteerMult);
	}
}

void resetSteeringMultiplier() {
	if (vehicle != 0) {
		if (ext.GetSteeringMultiplier(vehicle) != 1.0f) {
			ext.SetSteeringMultiplier(vehicle, 1.0f);
		}
	}
}

void updateLastInputDevice() {
	if (controls.PrevInput != controls.GetLastInputDevice(controls.PrevInput,settings.EnableWheel)) {
		controls.PrevInput = controls.GetLastInputDevice(controls.PrevInput, settings.EnableWheel);
		switch (controls.PrevInput) {
			case ScriptControls::Keyboard:
				showNotification("Switched to keyboard/mouse", &prevNotification);
				break;
			case ScriptControls::Controller: // Controller
				if (settings.ShiftMode == HPattern) {
					showNotification("Switched to controller\nSequential re-initiated", &prevNotification);
					settings.ShiftMode = Sequential;
				}
				else {
					showNotification("Switched to controller", &prevNotification);
				}
				break;
			case ScriptControls::Wheel:
				showNotification("Switched to wheel", &prevNotification);
				break;
			//case ScriptControls::Stick: 
			//	showNotification("Switched to stick", &prevNotification);
			//	break;
			default: break;
		}
		initSteeringPatches();
	}
	if (controls.PrevInput == ScriptControls::Wheel) {
		CONTROLS::STOP_PAD_SHAKE(0);
	}
}

void updateSteeringMultiplier() {
	ext.SetSteeringMultiplier(vehicle, settings.GameSteerMult);
}

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Shifting
///////////////////////////////////////////////////////////////////////////////

void setShiftMode(int shiftMode) {
	gearRattle.Stop();
	if (shiftMode > 2 || shiftMode < 0)
		return;

	if (settings.ShiftMode == HPattern  && controls.PrevInput == ScriptControls::Controller) {
		settings.ShiftMode = Automatic;
	}

	if ((settings.ShiftMode == Automatic || settings.ShiftMode == Sequential) && vehData.CurrGear > 1) {
		vehData.SimulatedNeutral = false;
	}

	std::string mode = "Mode: ";
	// ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
	switch (settings.ShiftMode) {
		case Sequential: mode += "Sequential"; break;
		case HPattern: mode += "H-Pattern"; break;
		case Automatic: mode += "Automatic"; break;
		default: break;
	}
	showNotification(mode, &prevNotification);
}

void cycleShiftMode() {
	++settings.ShiftMode;
	if (settings.ShiftMode >= SIZEOF_ShiftModes) {
		settings.ShiftMode = (ShiftModes)0;
	}

	setShiftMode(settings.ShiftMode);
	settings.SaveGeneral();
}

void shiftTo(int gear, bool autoClutch) {
	if (autoClutch) {
		controls.ClutchVal = 1.0f;
		CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
	}
	vehData.LockGear = gear;
	vehData.LockTruck = false;
	if (vehData.IsTruck && vehData.Rpm < 0.9) {
		return;
	}
	vehData.PrevGear = vehData.CurrGear;
}
void functionHShiftTo(int i) {
	if (settings.ClutchShiftingH && !vehData.NoClutch) {
		if (controls.ClutchVal > 1.0f - settings.ClutchCatchpoint) {
			shiftTo(i, false);
			vehData.SimulatedNeutral = false;
			gearRattle.Stop();
		}
		else {
			gearRattle.Play(vehicle);
			vehData.SimulatedNeutral = true;
			if (settings.EngDamage &&
				!vehData.NoClutch) {
				VEHICLE::SET_VEHICLE_ENGINE_HEALTH(
					vehicle,
					VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle) - settings.MisshiftDamage);
			}
		}
	}
	else {
		shiftTo(i, true);
		vehData.SimulatedNeutral = false;
		gearRattle.Stop();
	}
}

void functionHShiftKeyboard() {
	// highest vehicle gear is 7th
	int clamp = 7;
	if (vehData.TopGear <= clamp) {
		clamp = vehData.TopGear;
	}
	for (uint8_t i = 0; i <= clamp; i++) {
		if (controls.ButtonJustPressed(static_cast<ScriptControls::KeyboardControlType>(i))) {
			functionHShiftTo(i);
		}
	}
	if (controls.ButtonJustPressed(ScriptControls::KeyboardControlType::HN) &&
		!vehData.NoClutch) {
		vehData.SimulatedNeutral = !vehData.SimulatedNeutral;
	}
}

void functionHShiftWheel() {
	// highest vehicle gear is 7th
	int clamp = 7;
	if (vehData.TopGear <= clamp) {
		clamp = vehData.TopGear;
	}
	for (uint8_t i = 0; i <= clamp; i++) {
		if (controls.ButtonJustPressed(static_cast<ScriptControls::WheelControlType>(i))) {
			functionHShiftTo(i);
		}
	}
	// Bleh
	if (controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::H1)) ||
		controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::H2)) ||
		controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::H3)) ||
		controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::H4)) ||
		controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::H5)) ||
		controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::H6)) ||
		controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::H7)) ||
		controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::HR))
	) {
		if (settings.ClutchShiftingH &&
			settings.EngDamage &&
			!vehData.NoClutch) {
			if (controls.ClutchVal < 1.0 - settings.ClutchCatchpoint) {
				gearRattle.Play(vehicle);
			}
		}
		vehData.SimulatedNeutral = !vehData.NoClutch;
	}

	if (controls.ButtonReleased(static_cast<ScriptControls::WheelControlType>(ScriptControls::WheelControlType::HR))) {
		shiftTo(1, true);
		vehData.SimulatedNeutral = !vehData.NoClutch;
	}
}

void functionSShift() {
	auto xcTapStateUp = controls.ButtonTapped(ScriptControls::ControllerControlType::ShiftUp);
	auto xcTapStateDn = controls.ButtonTapped(ScriptControls::ControllerControlType::ShiftDown);

	auto ncTapStateUp = controls.ButtonTapped(ScriptControls::LegacyControlType::ShiftUp);
	auto ncTapStateDn = controls.ButtonTapped(ScriptControls::LegacyControlType::ShiftDown);

	// Shift up
	if (controls.PrevInput == ScriptControls::Controller	&& xcTapStateUp == XboxController::TapState::Tapped ||
		controls.PrevInput == ScriptControls::Controller	&& ncTapStateUp == LegacyController::TapState::Tapped ||
		controls.PrevInput == ScriptControls::Keyboard		&& controls.ButtonJustPressed(ScriptControls::KeyboardControlType::ShiftUp) ||
		controls.PrevInput == ScriptControls::Wheel			&& controls.ButtonJustPressed(ScriptControls::WheelControlType::ShiftUp)) {
		if (vehData.NoClutch) {
			if (vehData.CurrGear < vehData.TopGear) {
				shiftTo(vehData.LockGear + 1, true);
			}
			return;
		}

		// Shift block /w clutch shifting for seq.
		if (settings.ClutchShiftingS && 
			controls.ClutchVal < 1.0f - settings.ClutchCatchpoint) {
			return;
		}

		// Reverse to Neutral
		if (vehData.CurrGear == 0 && !vehData.SimulatedNeutral) {
			shiftTo(1, true);
			vehData.SimulatedNeutral = true;
			return;
		}

		// Neutral to 1
		if (vehData.CurrGear == 1 && vehData.SimulatedNeutral) {
			vehData.SimulatedNeutral = false;
			return;
		}

		// 1 to X
		if (vehData.CurrGear < vehData.TopGear) {
			shiftTo(vehData.LockGear + 1, true);
		}
	}

	// Shift down

	if (controls.PrevInput == ScriptControls::Controller	&& xcTapStateDn == XboxController::TapState::Tapped ||
		controls.PrevInput == ScriptControls::Controller	&& ncTapStateDn == LegacyController::TapState::Tapped ||
		controls.PrevInput == ScriptControls::Keyboard		&& controls.ButtonJustPressed(ScriptControls::KeyboardControlType::ShiftDown) ||
		controls.PrevInput == ScriptControls::Wheel			&& controls.ButtonJustPressed(ScriptControls::WheelControlType::ShiftDown)) {
		if (vehData.NoClutch) {
			if (vehData.CurrGear > 0) {
				shiftTo(vehData.LockGear - 1, true);
			}
			return;
		}

		// Shift block /w clutch shifting for seq.
		if (settings.ClutchShiftingS &&
			controls.ClutchVal < 1.0f - settings.ClutchCatchpoint) {
			return;
		}

		// 1 to Neutral
		if (vehData.CurrGear == 1 && !vehData.SimulatedNeutral) {
			vehData.SimulatedNeutral = true;
			return;
		}

		// Neutral to R
		if (vehData.CurrGear == 1 && vehData.SimulatedNeutral) {
			shiftTo(0, true);
			vehData.SimulatedNeutral = false;
			return;
		}

		// X to 1
		if (vehData.CurrGear > 1) {
			shiftTo(vehData.LockGear - 1, true);
		}
	}
}

void functionAShift() { // Automatic
	auto xcTapStateUp = controls.ButtonTapped(ScriptControls::ControllerControlType::ShiftUp);
	auto xcTapStateDn = controls.ButtonTapped(ScriptControls::ControllerControlType::ShiftDown);

	auto ncTapStateUp = controls.ButtonTapped(ScriptControls::LegacyControlType::ShiftUp);
	auto ncTapStateDn = controls.ButtonTapped(ScriptControls::LegacyControlType::ShiftDown);

	// Shift up
	if (controls.PrevInput == ScriptControls::Controller	&& xcTapStateUp == XboxController::TapState::Tapped ||
		controls.PrevInput == ScriptControls::Controller	&& ncTapStateUp == LegacyController::TapState::Tapped ||
		controls.PrevInput == ScriptControls::Keyboard		&& controls.ButtonJustPressed(ScriptControls::KeyboardControlType::ShiftUp) ||
		controls.PrevInput == ScriptControls::Wheel			&& controls.ButtonJustPressed(ScriptControls::WheelControlType::ShiftUp)) {
		// Reverse to Neutral
		if (vehData.CurrGear == 0 && !vehData.SimulatedNeutral) {
			shiftTo(1, true);
			vehData.SimulatedNeutral = true;
			return;
		}

		// Neutral to 1
		if (vehData.CurrGear == 1 && vehData.SimulatedNeutral) {
			vehData.SimulatedNeutral = false;
			return;
		}
	}

	// Shift down

	if (controls.PrevInput == ScriptControls::Controller	&& xcTapStateDn == XboxController::TapState::Tapped ||
		controls.PrevInput == ScriptControls::Controller	&& ncTapStateDn == LegacyController::TapState::Tapped ||
		controls.PrevInput == ScriptControls::Keyboard		&& controls.ButtonJustPressed(ScriptControls::KeyboardControlType::ShiftDown) ||
		controls.PrevInput == ScriptControls::Wheel			&& controls.ButtonJustPressed(ScriptControls::WheelControlType::ShiftDown)) {
		// 1 to Neutral
		if (vehData.CurrGear == 1 && !vehData.SimulatedNeutral) {
			vehData.SimulatedNeutral = true;
			return;
		}

		// Neutral to R
		if (vehData.CurrGear == 1 && vehData.SimulatedNeutral) {
			shiftTo(0, true);
			vehData.SimulatedNeutral = false;
			return;
		}
	}

	// Shift up
	if (vehData.CurrGear > 0 &&
		(vehData.CurrGear < vehData.NextGear && vehData.Speed > 2.0f)) {
		if (ext.GetDashSpeed(vehicle) > upshiftSpeeds[vehData.CurrGear])
			upshiftSpeeds[vehData.CurrGear] = ext.GetDashSpeed(vehicle);

		shiftTo(vehData.CurrGear + 1, true);
		vehData.SimulatedNeutral = false;
	}

	// Shift down
	if (vehData.CurrGear > 1) {
		auto ratios = ext.GetGearRatios(vehicle);
		float DriveMaxFlatVel = ext.GetDriveMaxFlatVel(vehicle);
		float prevGearRedline = DriveMaxFlatVel / ratios[vehData.CurrGear - 1];
		float velDiff = prevGearRedline - ext.GetDashSpeed(vehicle);
		if (velDiff > + 8.0f + 7.0f*(0.5f - controls.ThrottleVal)) {
			shiftTo(vehData.CurrGear - 1, true);
			vehData.NextGear = vehData.CurrGear - 1;
			vehData.SimulatedNeutral = false;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Gearbox features
///////////////////////////////////////////////////////////////////////////////

void functionClutchCatch() {
	float lowSpeed = 2.5f;
	float idleThrottle = 0.45f;

	if (controls.ClutchVal < 1.0f - settings.ClutchCatchpoint) {
		if (settings.ShiftMode != HPattern && controls.BrakeVal > 0.1f || 
			vehData.Rpm > 0.25f && vehData.Speed >= lowSpeed) {
			return;
		}

		// Forward
		if (vehData.CurrGear > 0 && vehData.Speed < vehData.CurrGear * lowSpeed &&
		    controls.ThrottleVal < 0.25f && controls.BrakeVal < 0.95) {
			CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, idleThrottle);
			ext.SetCurrentRPM(vehicle, 0.21f);
			ext.SetThrottle(vehicle, 0.0f);

		}

		// Reverse
		if (vehData.CurrGear == 0 &&
			controls.ThrottleVal < 0.25f && controls.BrakeVal < 0.95) {
			if (vehData.Velocity < -lowSpeed) {
				controls.ClutchVal = 1.0f;
			}
			CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, idleThrottle);
			ext.SetCurrentRPM(vehicle, 0.21f);
			ext.SetThrottle(vehicle, 0.0f);
		}
	}
}

std::vector<bool> getWheelLockups(Vehicle handle) {
	std::vector<bool> lockups;
	float velocity = ENTITY::GET_ENTITY_VELOCITY(vehicle).y;
	auto wheelsSpeed = ext.GetWheelRotationSpeeds(vehicle);
	for (auto wheelSpeed : wheelsSpeed) {
		if (abs(velocity) > 0.01f && wheelSpeed == 0.0f)
			lockups.push_back(true);
		else
			lockups.push_back(false);
	}
	return lockups;
}

void drawVehicleWheelInfo() {
	auto numWheels = ext.GetNumWheels(vehicle);
	auto wheelsSpeed = ext.GetTyreSpeeds(vehicle);
	auto wheelsCompr = ext.GetWheelCompressions(vehicle);
	auto wheelsHealt = ext.GetWheelHealths(vehicle);
	auto wheelsContactCoords = ext.GetWheelLastContactCoords(vehicle);
	auto wheelsOnGround = ext.GetWheelsOnGround(vehicle);
	auto wheelCoords = ext.GetWheelCoords(vehicle, ENTITY::GET_ENTITY_COORDS(vehicle, true), ENTITY::GET_ENTITY_ROTATION(vehicle, 0), ENTITY::GET_ENTITY_FORWARD_VECTOR(vehicle));
	auto wheelLockups = getWheelLockups(vehicle);

	for (int i = 0; i < numWheels; i++) {
		float wheelSpeed = wheelsSpeed[i];
		float wheelCompr = wheelsCompr[i];
		float wheelHealt = wheelsHealt[i];
		Color c = wheelLockups[i] ? solidOrange : transparentGray;
		c = wheelsOnGround[i] ? c : solidRed;
		showDebugInfo3D(wheelCoords[i], {
			"Speed: " + std::to_string(wheelSpeed),
			"Compress: " + std::to_string(wheelCompr),
			"Health: " + std::to_string(wheelHealt), },
			c );
		GRAPHICS::DRAW_LINE(wheelCoords[i].x, wheelCoords[i].y, wheelCoords[i].z, 
							wheelCoords[i].x, wheelCoords[i].y, wheelCoords[i].z + 1.0f + 2.5f * wheelCompr, 255, 0, 0, 255);
	}
}

std::vector<float> getDrivenWheelsSpeeds(std::vector<float> wheelSpeeds) {
	std::vector<float> wheelsToConsider;
	if (vehData.DriveBiasFront > 0.0f && vehData.DriveBiasFront < 1.0f) {
		wheelsToConsider = wheelSpeeds;
	}
	else {
		// bikes
		if (ext.GetNumWheels(vehicle) == 2) {
			// fwd
			if (vehData.DriveBiasFront == 1.0f) {
				wheelsToConsider.push_back(wheelSpeeds[0]);
			}
			// rwd
			else if (vehData.DriveBiasFront == 0.0f) {
				wheelsToConsider.push_back(wheelSpeeds[1]);
			}
		}
		// normal cars
		else if (ext.GetNumWheels(vehicle) == 4) {
			// fwd
			if (vehData.DriveBiasFront == 1.0f) {
				wheelsToConsider.push_back(wheelSpeeds[0]);
				wheelsToConsider.push_back(wheelSpeeds[1]);
			}
			// rwd
			else if (vehData.DriveBiasFront == 0.0f) {
				wheelsToConsider.push_back(wheelSpeeds[2]);
				wheelsToConsider.push_back(wheelSpeeds[3]);
			}
		}
		// offroad, trucks
		else if (ext.GetNumWheels(vehicle) == 6) {
			// fwd
			if (vehData.DriveBiasFront == 1.0f) {
				wheelsToConsider.push_back(wheelSpeeds[0]);
				wheelsToConsider.push_back(wheelSpeeds[1]);
			}
			// rwd
			else if (vehData.DriveBiasFront == 0.0f) {
				wheelsToConsider.push_back(wheelSpeeds[2]);
				wheelsToConsider.push_back(wheelSpeeds[3]);
				wheelsToConsider.push_back(wheelSpeeds[4]);
				wheelsToConsider.push_back(wheelSpeeds[5]);
			}
		}
		else {
			wheelsToConsider = wheelSpeeds;
		}
	}
	return wheelsToConsider;
}

void functionEngStall() {
	float lowSpeed = 2.4f;
	float stallingRateDivisor = 3500000.0f;
	float timeStep = SYSTEM::TIMESTEP() * 100.0f;
	float avgWheelSpeed = abs(avg(getDrivenWheelsSpeeds(ext.GetTyreSpeeds(vehicle))));
	if (vehData.Clutch > 0.2f && // 1.0 = fully engaged, 0.1 = fully disengaged
		controls.ClutchVal < 1.0f - settings.StallingThreshold &&
		vehData.Rpm < 0.25f &&
		((avgWheelSpeed < vehData.CurrGear * lowSpeed) || (vehData.CurrGear == 0 && avgWheelSpeed < lowSpeed)) &&
		VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
		stallingProgress += (rand() % 1000) / (stallingRateDivisor * (controls.ThrottleVal+0.001f) * timeStep);
	}
	else {
		if (stallingProgress > 0.0f) {
			stallingProgress -= (rand() % 1000) / (stallingRateDivisor * (controls.ThrottleVal + 0.001f) * timeStep);
		}
	}
	if (stallingProgress > 1.0f) {
		VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, false, true, true);
		gearRattle.Stop();
		stallingProgress = 0.0f;
		if (settings.DisplayInfo)
			showNotification("Your car has stalled.");
	}
	//showText(0.1, 0.1, 1.0, ("Stall progress:" + std::to_string(stallingProgress)), 0, solidWhite, true);
	// todo: add engine startup when wheels roll + clutch
	// not sure if this is already in the game in some sorts
}

void functionEngDamage() {
	if (vehData.Rpm > 0.98f &&
		controls.ThrottleVal > 0.98f) {
		VEHICLE::SET_VEHICLE_ENGINE_HEALTH(vehicle, 
										   VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle) - (settings.RPMDamage));
	}
}

// im solving the worng problem here help
std::vector<bool> getDrivenWheels() {
	int numWheels = ext.GetNumWheels(vehicle);
	std::vector<bool> wheelsToConsider;
	if (vehData.DriveBiasFront > 0.0f && vehData.DriveBiasFront < 1.0f) {
		for (int i = 0; i < numWheels; i++)
			wheelsToConsider.push_back(true);
	}
	else {
		// bikes
		if (numWheels == 2) {
			// fwd
			if (vehData.DriveBiasFront == 1.0f) {
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(false);
			}
			// rwd
			else if (vehData.DriveBiasFront == 0.0f) {
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(true);
			}
		}
		// normal cars
		else if (numWheels == 4) {
			// fwd
			if (vehData.DriveBiasFront == 1.0f) {
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(false);
			}
			// rwd
			else if (vehData.DriveBiasFront == 0.0f) {
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(true);
			}
		}
		// offroad, trucks
		else if (numWheels == 6) {
			// fwd
			if (vehData.DriveBiasFront == 1.0f) {
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(false);
			}
			// rwd
			else if (vehData.DriveBiasFront == 0.0f) {
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(false);
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(true);
				wheelsToConsider.push_back(true);
			}
		}
		else {
			for (int i = 0; i < numWheels; i++)
				wheelsToConsider.push_back(true);
		}
	}
	return wheelsToConsider;
}

void functionEngLock() {

	// ok so this took me way too much time
	// I finally found the gearing ratio stuff, which along with DriveMaxFlatVel (?)
	// can be used to figure out the max speed for that gear.
	// Beware though as this might be different for trucks. Haven't checked it yet!
	float speed = ext.GetDashSpeed(vehicle);
	auto ratios = ext.GetGearRatios(vehicle);
	float DriveMaxFlatVel = ext.GetDriveMaxFlatVel(vehicle);
	float maxSpeed = DriveMaxFlatVel / ratios[vehData.CurrGear];

	// Wheels are locking up due to bad downshifts
	if (vehData.CurrGear != 0 && vehData.CurrGear != vehData.TopGear &&
		!vehData.SimulatedNeutral && speed > maxSpeed + 3.334f) {
		if (vehData.IsTruck && vehData.CurrGear == 1 &&
			speed < maxSpeed + 12.0f)
			return;
		if (vehData.IsTruck && vehData.CurrGear == 2 &&
			speed < maxSpeed + 6.0f)
			return;


		if (!MemoryPatcher::BrakeDecrementPatched) {
			MemoryPatcher::PatchBrakeDecrement();
		}
		float inputMultiplier = (1.0f - controls.ClutchVal);
		float lockingForce = 60.0f * inputMultiplier;
		auto wheelsToLock = getDrivenWheels();
		for (int i = 0; i < ext.GetNumWheels(vehicle); i++) {
			if (i >= wheelsToLock.size() || wheelsToLock[i]) {
				ext.SetWheelBrakePressure(vehicle, i, lockingForce);
				ext.SetWheelSkidSmokeEffect(vehicle, i, lockingForce);
			}
		}
		fakeRev(true, 1.0f);
		gearRattle.Play(vehicle);
		if (settings.EngDamage) {
			VEHICLE::SET_VEHICLE_ENGINE_HEALTH(
				vehicle,
				VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle) - settings.EngDamage);
		}
		if (settings.DisplayInfo) {
			showText(0.5, 0.45, 0.5, "Eng block @ " + std::to_string(static_cast<int>(inputMultiplier * 100.0f)) + "%");
			showText(0.5, 0.50, 0.5, "Eng block @ " + std::to_string(lockingForce));
		}
	}
	else {
		if (MemoryPatcher::BrakeDecrementPatched) {
			MemoryPatcher::RestoreBrakeDecrement();
		}
		gearRattle.Stop();
	}
}

void functionEngBrake() {
	// When you let go of the throttle at high RPMs
	float activeBrakeThreshold = settings.EngBrakeThreshold;
	if (vehData.Rpm >= activeBrakeThreshold && vehData.Velocity > 5.0f) {
		float throttleMultiplier = 1.0f - controls.ThrottleVal;
		float clutchMultiplier = 1.0f - controls.ClutchVal;
		float inputMultiplier = throttleMultiplier * clutchMultiplier;
		if (inputMultiplier > 0.0f) {
			if (!MemoryPatcher::BrakeDecrementPatched) {
				MemoryPatcher::PatchBrakeDecrement();
			}
			float rpmMultiplier = (vehData.Rpm - activeBrakeThreshold) / (1.0f - activeBrakeThreshold);
			float brakePressure = settings.EngBrakePower * inputMultiplier * rpmMultiplier;
			auto wheelsToBrake = getDrivenWheels();
			for (int i = 0; i < ext.GetNumWheels(vehicle); i++) {
				if (i >= wheelsToBrake.size() || wheelsToBrake[i]) {
					ext.SetWheelBrakePressure(vehicle, i, brakePressure);
				}
			}
			if (settings.DisplayInfo) {
				showText(0.5, 0.55, 0.5, "Eng brake @ " + std::to_string(static_cast<int>(inputMultiplier * 100.0f)) + "%");
				showText(0.5, 0.60, 0.5, "Pressure: " + std::to_string(brakePressure));
			}
		}
		
	}
	else {
		if (MemoryPatcher::BrakeDecrementPatched) {
			MemoryPatcher::RestoreBrakeDecrement();
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Gearbox control
///////////////////////////////////////////////////////////////////////////////

void fakeRev(bool customThrottle, float customThrottleVal) {
	float throttleVal = customThrottle ? customThrottleVal : controls.ThrottleVal;
	float timeStep = SYSTEM::TIMESTEP();
	float accelRatio = 2.5f * timeStep;
	float rpmValTemp = vehData.PrevRpm > vehData.Rpm ? vehData.PrevRpm - vehData.Rpm : 0.0f;
	if (vehData.CurrGear == 1) {			// For some reason, first gear revs slower
		rpmValTemp *= 2.0f;
	}
	float rpmVal = vehData.Rpm +			// Base value
		rpmValTemp +						// Keep it constant
		throttleVal * accelRatio;	// Addition value, depends on delta T
	//showText(0.4, 0.4, 2.0, "FakeRev");
	ext.SetCurrentRPM(vehicle, rpmVal);
}

void handleRPM() {
	float neutralClutchVal = -5.0f;
	float finalClutch = 0.0f;
	bool skip = false;

	// Game wants to shift up. Triggered at high RPM, high speed.
	// Desired result: high RPM, same gear, no more accelerating
	// Result:	Is as desired. Speed may drop a bit because of game clutch.
	// Update 2017-08-12: We know the gear speeds now, consider patching
	// shiftUp completely?
	if (vehData.CurrGear > 0 &&
		(vehData.CurrGear < vehData.NextGear && vehData.Speed > 2.0f)) {
		ext.SetThrottle(vehicle, 1.0f);
		fakeRev(false, 0);
		CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
		float counterForce = 0.25f*-(static_cast<float>(vehData.TopGear) - static_cast<float>(vehData.CurrGear))/static_cast<float>(vehData.TopGear);
		ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(vehicle, 1, 0.0f, counterForce, 0.0f, true, true, true, true);
		//showText(0.4, 0.1, 1.0, "REV LIM");
		//showText(0.4, 0.15, 1.0, "CF: " + std::to_string(counterForce));
	}

	// Emulate previous "shift down wanted" behavior.
	//if (vehData.CurrGear > 1 && vehData.Rpm < 0.4f) {
		// Don't artificially lower this any more
		//VEHICLE::_SET_VEHICLE_ENGINE_TORQUE_MULTIPLIER(vehicle, vehData.Rpm * 2.5f);
	//}

	/*
		Game doesn't rev on disengaged clutch in any gear but 1
		This workaround tries to emulate this
		Default: vehData.Clutch >= 0.6: Normal
		Default: vehData.Clutch < 0.6: Nothing happens
		Fix: Map 0.0-1.0 to 0.6-1.0 (clutchdata)
		Fix: Map 0.0-1.0 to 1.0-0.6 (control)
	*/
	if (vehData.CurrGear > 1) {
		// When pressing clutch and throttle, handle clutch and RPM
		if (controls.ClutchVal > 0.4f && 
			controls.ThrottleVal > 0.05f &&
		    !vehData.SimulatedNeutral && 
			// The next statement is a workaround for rolling back + brake + gear > 1 because it shouldn't rev then.
			// Also because we're checking on the game Control accel value and not the pedal position
			// TODO: Might wanna re-write with control.AccelVal instead of vehData.ControlAccelerate?
			!(vehData.Velocity < 0.0 && controls.BrakeVal > 0.1f && controls.ThrottleVal > 0.05f)) {
			fakeRev(false, 0);
			ext.SetThrottle(vehicle, controls.ThrottleVal);
			float tempVal = (1.0f - controls.ClutchVal) * 0.4f + 0.6f;
			if (controls.ClutchVal > 0.95) {
				tempVal = neutralClutchVal;
			}
			finalClutch = tempVal;
			skip = true;
		}
		// Don't care about clutch slippage, just handle RPM now
		if (vehData.SimulatedNeutral) {
			fakeRev(false, 0);
			ext.SetThrottle(vehicle, 1.0f); // For a fuller sound
		}
	}

	// Set the clutch depending on neutral status
	if (vehData.SimulatedNeutral || controls.ClutchVal > 1.0 - settings.ClutchCatchpoint) {
		/*
			To prevent a the clutch not being registered as fully pressed
			by the game. Negative values seem to work, but this amount
			differs from vehicle to vehicle, so it is at -0.1f to cover
			every case. Stronger negative values don't seem problematic.
			+Supercars seem to have a higher offset. -0.2f now,
			+Tuning the transmission messes this up, so just use -0.5f.
			+Still messes up the T20 @ Max Transmission upgrade.
		*/
		finalClutch = neutralClutchVal;
	}
	else {
		if (!skip) {
			finalClutch = 1.0f - controls.ClutchVal;
			//showText(0.1, 0.1, 0.5, "we have clutch");
		}
	}
	vehData.UpdateRpm();
	ext.SetClutch(vehicle, finalClutch);
	//showText(0.1, 0.15, 0.5, ("clutch set: " + std::to_string(finalClutch)));
}

/*
 * Truck gearbox code doesn't stop accelerating, so this speed limiter
 * is needed to stop acceleration once upshift point is reached.
 * TODO: Fix speed limiter. Use the same as the other limiter?
 */
void functionTruckLimiting() {
	// Save speed @ shift
	if (vehData.CurrGear < vehData.NextGear) {
		if (vehData.PrevGear <= vehData.CurrGear ||
			vehData.Velocity <= vehData.LockSpeed ||
			vehData.LockSpeed < 0.01f) {
			vehData.LockSpeed = vehData.Velocity;
		}
	}

	// Update speed
	if (vehData.CurrGear < vehData.NextGear) {
		vehData.LockSpeed = vehData.Velocity;
		vehData.LockTruck = true;
	}

	// Limit
	if ((vehData.Velocity > vehData.LockSpeed && vehData.LockTruck) ||
		(vehData.Velocity > vehData.LockSpeed && vehData.PrevGear > vehData.CurrGear)) {
		controls.ClutchVal = 1.0f;
		vehData.TruckShiftUp = true;
	}
	else {
		vehData.TruckShiftUp = false;
	}
}

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Reverse/Pedal handling
///////////////////////////////////////////////////////////////////////////////

// Anti-Deadzone.
void SetControlADZ(eControl control, float value, float adz) {
	CONTROLS::_SET_CONTROL_NORMAL(0, control, sgn(value)*adz+(1.0f-adz)*value);
}

void functionRealReverse() {
	// Forward gear
	// Desired: Only brake
	if (vehData.CurrGear > 0) {
		// LT behavior when stopped: Just brake
		if (controls.BrakeVal > 0.01f && controls.ThrottleVal < controls.BrakeVal &&
			vehData.Velocity < 0.5f && vehData.Velocity >= -0.5f) { // < 0.5 so reverse never triggers
																	//showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Stop");
			CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
			ext.SetThrottleP(vehicle, 0.1f);
			ext.SetBrakeP(vehicle, 1.0f);
			VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
		}
		// LT behavior when rolling back: Brake
		if (controls.BrakeVal > 0.01f && controls.ThrottleVal < controls.BrakeVal &&
			vehData.Velocity < -0.5f) {
			//showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Rollback");
			VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
			CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
			CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, controls.BrakeVal);
			ext.SetThrottle(vehicle, 0.0f);
			ext.SetThrottleP(vehicle, 0.1f);
			ext.SetBrakeP(vehicle, 1.0f);
		}
		// RT behavior when rolling back: Burnout
		if (controls.ThrottleVal > 0.5f && vehData.Velocity < -1.0f) {
			//showText(0.3, 0.3, 0.5, "functionRealReverse: Throttle @ Rollback");
			CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, controls.ThrottleVal);
			if (controls.BrakeVal < 0.1f) {
				VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, false);
			}
		}
	}
	// Reverse gear
	// Desired: RT reverses, LT brakes
	if (vehData.CurrGear == 0) {
		// Enables reverse lights
		ext.SetThrottleP(vehicle, -0.1f);
		// RT behavior
		int throttleAndSomeBrake = 0;
		if (controls.ThrottleVal > 0.01f && controls.ThrottleVal > controls.BrakeVal) {
			throttleAndSomeBrake++;
			//showText(0.3, 0.3, 0.5, "functionRealReverse: Throttle @ Active Reverse");

			CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
			CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, controls.ThrottleVal);
		}
		// LT behavior when reversing
		if (controls.BrakeVal > 0.01f &&
			vehData.Velocity <= -0.5f) {
			throttleAndSomeBrake++;
			//showText(0.3, 0.35, 0.5, "functionRealReverse: Brake @ Reverse");

			CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
			CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, controls.BrakeVal);
		}
		// Throttle > brake  && BrakeVal > 0.1f
		if (throttleAndSomeBrake >= 2) {
			//showText(0.3, 0.4, 0.5, "functionRealReverse: Weird combo + rev it");

			CONTROLS::ENABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
			CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, controls.BrakeVal);
			fakeRev(false, 0);
		}

		// LT behavior when forward
		if (controls.BrakeVal > 0.01f && controls.ThrottleVal <= controls.BrakeVal &&
			vehData.Velocity > 0.1f) {
			//showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Rollforwrd");

			VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
			CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, controls.BrakeVal);

			//CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
			ext.SetBrakeP(vehicle, 1.0f);
		}

		// LT behavior when still
		if (controls.BrakeVal > 0.01f && controls.ThrottleVal <= controls.BrakeVal &&
			vehData.Velocity > -0.5f && vehData.Velocity <= 0.1f) {
			//showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Stopped");

			VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
			CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
			ext.SetBrakeP(vehicle, 1.0f);
		}
	}
}

// Forward gear: Throttle accelerates, Brake brakes (exclusive)
// Reverse gear: Throttle reverses, Brake brakes (exclusive)
void handlePedalsRealReverse(float wheelThrottleVal, float wheelBrakeVal) {
	float speedThreshold = 0.5f;
	float reverseThreshold = 1.0f;

	if (vehData.CurrGear > 0) {
		// Going forward
		if (vehData.Velocity > speedThreshold) {
			//showText(0.3, 0.0, 1.0, "We are going forward");
			// Throttle Pedal normal
			if (wheelThrottleVal > 0.01f) {
				SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, controls.ADZThrottle);
			}
			// Brake Pedal normal
			if (wheelBrakeVal > 0.01f) {
				SetControlADZ(ControlVehicleBrake, wheelBrakeVal, controls.ADZBrake);
			}
		}

		// Standing still
		if (vehData.Velocity < speedThreshold && vehData.Velocity >= -speedThreshold) {
			//showText(0.3, 0.0, 1.0, "We are stopped");

			if (wheelThrottleVal > 0.01f) {
				SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, controls.ADZThrottle);
			}

			if (wheelBrakeVal > 0.01f) {
				ext.SetThrottleP(vehicle, 0.1f);
				ext.SetBrakeP(vehicle, 1.0f);
				VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
			}
		}

		// Rolling back
		if (vehData.Velocity < -speedThreshold) {
			bool brakelights = false;
			// Just brake
			if (wheelThrottleVal <= 0.01f && wheelBrakeVal > 0.01f) {
				//showText(0.3, 0.0, 1.0, "We should brake");
				//showText(0.3, 0.05, 1.0, ("Brake pressure:" + std::to_string(wheelBrakeVal)));
				SetControlADZ(ControlVehicleAccelerate, wheelBrakeVal, controls.ADZBrake);
				ext.SetThrottleP(vehicle, 0.1f);
				ext.SetBrakeP(vehicle, 1.0f);
				brakelights = true;
			}
			
			if (wheelThrottleVal > 0.01f && controls.ClutchVal < settings.ClutchCatchpoint && !vehData.SimulatedNeutral) {
				//showText(0.3, 0.0, 1.0, "We should burnout");
				SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, controls.ADZThrottle);
				SetControlADZ(ControlVehicleBrake, wheelThrottleVal, controls.ADZThrottle);
			}

			if (wheelThrottleVal > 0.01f && (controls.ClutchVal > settings.ClutchCatchpoint || vehData.SimulatedNeutral)) {
				if (wheelBrakeVal > 0.01f) {
					//showText(0.3, 0.0, 1.0, "We should rev and brake");
					//showText(0.3, 0.05, 1.0, ("Brake pressure:" + std::to_string(wheelBrakeVal)) );
					SetControlADZ(ControlVehicleAccelerate, wheelBrakeVal, controls.ADZBrake);
					ext.SetThrottleP(vehicle, 0.1f);
					ext.SetBrakeP(vehicle, 1.0f);
					brakelights = true;
					fakeRev(false, 0);
				}
				else if (controls.ClutchVal > 0.9 || vehData.SimulatedNeutral) {
					//showText(0.3, 0.0, 1.0, "We should rev and do nothing");
					ext.SetThrottleP(vehicle, wheelThrottleVal); 
					fakeRev(false, 0);
				} else {
					//showText(0.3, 0.0, 1.0, "We should rev and apply throttle");
					SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, controls.ADZThrottle);
					ext.SetThrottleP(vehicle, wheelThrottleVal);
					fakeRev(false, 0);
				}
			}
			VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, brakelights);
		}
	}

	if (vehData.CurrGear == 0) {
		// Enables reverse lights
		ext.SetThrottleP(vehicle, -0.1f);
		
		// We're reversing
		if (vehData.Velocity < -speedThreshold) {
			//showText(0.3, 0.0, 1.0, "We are reversing");
			// Throttle Pedal Reverse
			if (wheelThrottleVal > 0.01f) {
				SetControlADZ(ControlVehicleBrake, wheelThrottleVal, controls.ADZThrottle);
			}
			// Brake Pedal Reverse
			if (wheelBrakeVal > 0.01f) {
				SetControlADZ(ControlVehicleAccelerate, wheelBrakeVal, controls.ADZBrake);
				ext.SetThrottleP(vehicle, -wheelBrakeVal);
				ext.SetBrakeP(vehicle, 1.0f);
			}
		}

		// Standing still
		if (vehData.Velocity < speedThreshold && vehData.Velocity >= -speedThreshold) {
			//showText(0.3, 0.0, 1.0, "We are stopped");

			if (wheelThrottleVal > 0.01f) {
				SetControlADZ(ControlVehicleBrake, wheelThrottleVal, controls.ADZThrottle);
			}

			if (wheelBrakeVal > 0.01f) {
				ext.SetThrottleP(vehicle, -wheelBrakeVal);
				ext.SetBrakeP(vehicle, 1.0f);
				VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
			}
		}

		// We're rolling forwards
		if (vehData.Velocity > speedThreshold) {
			//showText(0.3, 0.0, 1.0, "We are rolling forwards");
			//bool brakelights = false;

			if (vehData.Velocity > reverseThreshold * 1.0f) {
				gearRattle.Play(vehicle);
				//shiftTo(1, false);
				//vehData.SimulatedNeutral = true;
				
				if (controls.ClutchVal < settings.ClutchCatchpoint) {
					CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHandbrake, 1.0f);
				}
				if (settings.EngDamage) {
					VEHICLE::SET_VEHICLE_ENGINE_HEALTH(
						vehicle,
						VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle) - settings.RPMDamage * 2);
				}
				// Brake Pedal Reverse
				if (wheelBrakeVal > 0.01f) {
					SetControlADZ(ControlVehicleBrake, wheelBrakeVal, controls.ADZBrake);
					ext.SetThrottleP(vehicle, -wheelBrakeVal);
					ext.SetBrakeP(vehicle, 1.0f);
				}
				//showNotification("Woops", nullptr);
			}

			//VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, brakelights);
		}
	}
}

// Pedals behave like RT/LT
void handlePedalsDefault(float wheelThrottleVal, float wheelBrakeVal) {
	if (wheelThrottleVal > 0.01f) {
		SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, controls.ADZThrottle);
	}
	if (wheelBrakeVal > 0.01f) {
		SetControlADZ(ControlVehicleBrake, wheelBrakeVal, controls.ADZBrake);
	}
}

void functionAutoReverse() {
	// Go forward
	if (CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleAccelerate) && 
		!CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleBrake) &&
	    vehData.Velocity > -1.0f &&
	    vehData.CurrGear == 0) {
		vehData.LockGear = 1;
	}

	// Reverse
	if (CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleBrake) && 
		!CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleAccelerate) &&
	    vehData.Velocity < 1.0f &&
	    vehData.CurrGear > 0) {
		vehData.SimulatedNeutral = false;
		vehData.LockGear = 0;
	}
}

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Buttons
///////////////////////////////////////////////////////////////////////////////
// Blocks some vehicle controls on tapping, tries to activate them when holding.
// TODO: Some original "tap" controls don't work.
void blockButtons() {
	if (settings.BlockCarControls && settings.EnableManual && controls.PrevInput == ScriptControls::Controller &&
		!(settings.ShiftMode == Automatic && vehData.CurrGear > 1)) {

		if (controls.UseLegacyController) {
			for (int i = 0; i < static_cast<int>(ScriptControls::LegacyControlType::SIZEOF_LegacyControlType); i++) {
				if (controls.ControlNativeBlocks[i] == -1) continue;
				if (i != (int)ScriptControls::LegacyControlType::ShiftUp && i != (int)ScriptControls::LegacyControlType::ShiftDown) continue;

				if (controls.ButtonHeldOver(static_cast<ScriptControls::LegacyControlType>(i), 200)) {
					CONTROLS::_SET_CONTROL_NORMAL(0, controls.ControlNativeBlocks[i], 1.0f);
				}
				else {
					CONTROLS::DISABLE_CONTROL_ACTION(0, controls.ControlNativeBlocks[i], true);
				}

				if (controls.ButtonReleasedAfter(static_cast<ScriptControls::LegacyControlType>(i), 200)) {
					CONTROLS::_SET_CONTROL_NORMAL(0, controls.ControlNativeBlocks[i], 0.0f);
					CONTROLS::_SET_CONTROL_NORMAL(0, controls.ControlNativeBlocks[i], 1.0f);
				}
			}
		}
		else {
			for (int i = 0; i < static_cast<int>(ScriptControls::ControllerControlType::SIZEOF_ControllerControlType); i++) {
				if (controls.ControlXboxBlocks[i] == -1) continue;
				if (i != (int)ScriptControls::ControllerControlType::ShiftUp && i != (int)ScriptControls::ControllerControlType::ShiftDown) continue;

				if (controls.ButtonHeldOver(static_cast<ScriptControls::ControllerControlType>(i), 200)) {
					CONTROLS::_SET_CONTROL_NORMAL(0, controls.ControlXboxBlocks[i], 1.0f);
				}
				else {
					CONTROLS::DISABLE_CONTROL_ACTION(0, controls.ControlXboxBlocks[i], true);
				}

				if (controls.ButtonReleasedAfter(static_cast<ScriptControls::ControllerControlType>(i), 200)) {
					CONTROLS::_SET_CONTROL_NORMAL(0, controls.ControlXboxBlocks[i], 0.0f);
					CONTROLS::_SET_CONTROL_NORMAL(0, controls.ControlXboxBlocks[i], 1.0f);
				}
			}
		}
	}
}

void startStopEngine() {
	if (!VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle) &&
		(controls.PrevInput == ScriptControls::Controller	&& controls.ButtonJustPressed(ScriptControls::ControllerControlType::Engine) ||
		controls.PrevInput == ScriptControls::Controller	&& controls.ButtonJustPressed(ScriptControls::LegacyControlType::Engine) ||
		controls.PrevInput == ScriptControls::Keyboard		&& controls.ButtonJustPressed(ScriptControls::KeyboardControlType::Engine) ||
		controls.PrevInput == ScriptControls::Wheel			&& controls.ButtonJustPressed(ScriptControls::WheelControlType::Engine) ||
		settings.ThrottleStart && controls.ThrottleVal > 0.75f && (controls.ClutchVal > settings.ClutchCatchpoint || vehData.SimulatedNeutral))) {
		VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, true, false, true);
	}
	if (VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle) &&
		((controls.PrevInput == ScriptControls::Controller	&& controls.ButtonJustPressed(ScriptControls::ControllerControlType::Engine) && settings.ToggleEngine) ||
		(controls.PrevInput == ScriptControls::Controller	&& controls.ButtonJustPressed(ScriptControls::LegacyControlType::Engine) && settings.ToggleEngine) ||
		controls.PrevInput == ScriptControls::Keyboard		&& controls.ButtonJustPressed(ScriptControls::KeyboardControlType::Engine) ||
		controls.PrevInput == ScriptControls::Wheel			&& controls.ButtonJustPressed(ScriptControls::WheelControlType::Engine))) {
		VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, false, true, true);
	}
}

void handleVehicleButtons() {
	blockButtons();
	startStopEngine();

	if (!controls.WheelControl.IsConnected(controls.SteerGUID) ||
		controls.PrevInput != ScriptControls::Wheel) {
		return;
	}

	if (controls.HandbrakeVal > 0.1f) {
		CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHandbrake, controls.HandbrakeVal);
	}
	if (controls.ButtonIn(ScriptControls::WheelControlType::Handbrake)) {
		CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHandbrake, 1.0f);
	}
	if (controls.ButtonIn(ScriptControls::WheelControlType::Horn)) {
		CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHorn, 1.0f);
	}
	if (controls.ButtonJustPressed(ScriptControls::WheelControlType::Lights)) {
		CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHeadlight, 1.0f);
	}
	if (controls.ButtonIn(ScriptControls::WheelControlType::LookBack)) {
		CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleLookBehind, 1.0f);
	}
	
	// who was first?
	if (controls.ButtonIn(ScriptControls::WheelControlType::LookRight) &&
		controls.ButtonJustPressed(ScriptControls::WheelControlType::LookLeft)) {
		lookrightfirst = true;
	}

	if (controls.ButtonIn(ScriptControls::WheelControlType::LookLeft) &&
		controls.ButtonIn(ScriptControls::WheelControlType::LookRight)) {
		CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(lookrightfirst ? -180.0f : 180.0f);
	}
	else if (controls.ButtonIn(ScriptControls::WheelControlType::LookLeft)) {
		CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(90);
	}
	else if (controls.ButtonIn(ScriptControls::WheelControlType::LookRight)) {
		CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(-90);
	}
	if (controls.ButtonReleased(ScriptControls::WheelControlType::LookLeft) && !(controls.ButtonIn(ScriptControls::WheelControlType::LookRight)) ||
		controls.ButtonReleased(ScriptControls::WheelControlType::LookRight) && !(controls.ButtonIn(ScriptControls::WheelControlType::LookLeft))) {
		CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0);
	}
	if (controls.ButtonReleased(ScriptControls::WheelControlType::LookLeft)  ||
		controls.ButtonReleased(ScriptControls::WheelControlType::LookRight)) {
		lookrightfirst = false;
	}

	if (controls.ButtonJustPressed(ScriptControls::WheelControlType::Camera)) {
		CONTROLS::_SET_CONTROL_NORMAL(0, ControlNextCamera, 1.0f);
	}


	if (controls.ButtonHeld(ScriptControls::WheelControlType::RadioPrev) ||
		controls.ButtonHeld(ScriptControls::WheelControlType::RadioNext)) {
		if (AUDIO::GET_PLAYER_RADIO_STATION_INDEX() != RadioOff) {
			vehData.RadioStationIndex = AUDIO::GET_PLAYER_RADIO_STATION_INDEX();
		}
		AUDIO::SET_VEH_RADIO_STATION(vehicle, "OFF");
		return;
	}
	if (controls.ButtonReleased(ScriptControls::WheelControlType::RadioNext)) {
		if (AUDIO::GET_PLAYER_RADIO_STATION_INDEX() == RadioOff) {
			AUDIO::SET_RADIO_TO_STATION_INDEX(vehData.RadioStationIndex);
			return;
		}
		AUDIO::_0xFF266D1D0EB1195D(); // Next radio station
	}
	if (controls.ButtonReleased(ScriptControls::WheelControlType::RadioPrev)) {
		if (AUDIO::GET_PLAYER_RADIO_STATION_INDEX() == RadioOff) {
			AUDIO::SET_RADIO_TO_STATION_INDEX(vehData.RadioStationIndex);
			return;
		}
		AUDIO::_0xDD6BCF9E94425DF9(); // Prev radio station
	}

	if (controls.ButtonJustPressed(ScriptControls::WheelControlType::IndicatorLeft)) {
		if (!vehData.BlinkerLeft) {
			vehData.BlinkerTicks = 1;
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false); // L
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, true); // R
			vehData.BlinkerLeft = true;
			vehData.BlinkerRight = false;
			vehData.BlinkerHazard = false;
		}
		else {
			vehData.BlinkerTicks = 0;
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false);
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false);
			vehData.BlinkerLeft = false;
			vehData.BlinkerRight = false;
			vehData.BlinkerHazard = false;
		}
	}
	if (controls.ButtonJustPressed(ScriptControls::WheelControlType::IndicatorRight)) {
		if (!vehData.BlinkerRight) {
			vehData.BlinkerTicks = 1;
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, true); // L
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false); // R
			vehData.BlinkerLeft = false;
			vehData.BlinkerRight = true;
			vehData.BlinkerHazard = false;
		}
		else {
			vehData.BlinkerTicks = 0;
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false);
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false);
			vehData.BlinkerLeft = false;
			vehData.BlinkerRight = false;
			vehData.BlinkerHazard = false;
		}
	}

	float wheelCenterDeviation = ( controls.SteerVal - 0.5f) / 0.5f;

	if (vehData.BlinkerTicks == 1 && abs(wheelCenterDeviation) > 0.2f)
	{
		vehData.BlinkerTicks = 2;
	}

	if (vehData.BlinkerTicks == 2 && abs(wheelCenterDeviation) < 0.1f)
	{
		vehData.BlinkerTicks = 0;
		VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false);
		VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false);
		vehData.BlinkerLeft = false;
		vehData.BlinkerRight = false;
		vehData.BlinkerHazard = false;
	}

	if (controls.ButtonJustPressed(ScriptControls::WheelControlType::IndicatorHazard)) {
		if (!vehData.BlinkerHazard) {
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, true); // L
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, true); // R
			vehData.BlinkerLeft = false;
			vehData.BlinkerRight = false;
			vehData.BlinkerHazard = true;
		}
		else {
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false);
			VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false);
			vehData.BlinkerLeft = false;
			vehData.BlinkerRight = false;
			vehData.BlinkerHazard = false;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
//                    Wheel input and force feedback
///////////////////////////////////////////////////////////////////////////////

void doWheelSteering() {
	if (controls.PrevInput != ScriptControls::Wheel)
		return;

	float steerMult;
	if (vehData.Class == VehicleData::VehicleClass::Bike || vehData.Class == VehicleData::VehicleClass::Quad)
		steerMult = settings.SteerAngleMax / settings.SteerAngleBike;
	else if (vehData.Class == VehicleData::VehicleClass::Car)
		steerMult = settings.SteerAngleMax / settings.SteerAngleCar;
	else {
		steerMult = settings.SteerAngleMax / settings.SteerAngleBoat;
	}

	float effSteer = steerMult * 2.0f * (controls.SteerVal - 0.5f);

	/*
	 * Patched steering is direct without any processing, and super direct.
	 * _SET_CONTROL_NORMAL is with game processing and could have a bit of delay
	 * Both should work without any deadzone, with a note that the second one
	 * does need a specified anti-deadzone (recommended: 24-25%)
	 * 
	 */
	if (vehData.Class == VehicleData::VehicleClass::Car && settings.PatchSteeringControl) {
		ext.SetSteeringInputAngle(vehicle, -effSteer);
	}
	else {
		SetControlADZ(ControlVehicleMoveLeftRight, effSteer, controls.ADZSteer);
	}
}

void doStickControlAir() {
	SetControlADZ(ControlVehicleFlyThrottleUp, controls.PlaneThrottle, 0.25f);
	SetControlADZ(ControlVehicleFlyPitchUpDown, controls.PlanePitch, 0.25f);
	SetControlADZ(ControlVehicleFlyRollLeftRight, controls.PlaneRoll, 0.25f);
	SetControlADZ(ControlVehicleFlyYawLeft, controls.PlaneRudderL, 0.25f);
	SetControlADZ(ControlVehicleFlyYawRight, controls.PlaneRudderR, 0.25f);
}

void playFFBAir() {
	if (!settings.EnableFFB) {
		return;
	}
	// Stick ffb?
}

void playFFBGround(bool airborne, bool ignoreSpeed) {
	if (!settings.EnableFFB ||
		controls.PrevInput != ScriptControls::Wheel ||
		!controls.WheelControl.IsConnected(controls.SteerGUID)) {
		return;
	}

	if (settings.LogiLEDs) {
		controls.WheelControl.PlayLedsDInput(controls.SteerGUID, vehData.Rpm, 0.45, 0.95);
	}

	vehData.getAccelerationVectors(vehData.V3Velocities);
	Vector3 accelValsAvg = vehData.getAccelerationVectorsAverage();

	float steerMult;
	if (vehData.Class == VehicleData::VehicleClass::Bike || vehData.Class == VehicleData::VehicleClass::Quad)
		steerMult = settings.SteerAngleMax / settings.SteerAngleBike;
	else if (vehData.Class == VehicleData::VehicleClass::Car)
		steerMult = settings.SteerAngleMax / settings.SteerAngleCar;
	else {
		steerMult = settings.SteerAngleMax / settings.SteerAngleBoat;
	}
	float effSteer = steerMult * 2.0f * (controls.SteerVal - 0.5f);
	
	// targetSpeed is the speed at which the damperForce is at minimum
	// damperForce is maximum at speed 0 and decreases with speed increase
	float adjustRatio = static_cast<float>(settings.DamperMax) / static_cast<float>(settings.TargetSpeed);
	int damperForce = settings.DamperMax - static_cast<int>(vehData.Speed * adjustRatio);

	// Acceleration also affects damper force
	damperForce -= static_cast<int>(adjustRatio * accelValsAvg.y);
	
	if (damperForce < settings.DamperMin) {
		damperForce = settings.DamperMin;
	}

	// steerSpeed is to dampen the steering wheel
	auto steerAxis = controls.WheelControl.StringToAxis(controls.WheelAxes[static_cast<int>(controls.SteerAxisType)]);
	auto steerSpeed = controls.WheelControl.GetAxisSpeed(steerAxis, controls.SteerGUID) / 20;

	/*                    a                                        v^2
	 * Because G Force = ---- and a = v * omega, verified with a = ---   using a speedo and 
	 *                   9.81                                       r    r = ypg205t16a length
	 * No need to crappily emulate centering any more.
	 * Do: Find something for self-aligning torque
	 */
	double GForce = (vehData.RotationVelocity.z * vehData.Velocity) / 9.81f;

	// Oversteer  - when rear  wheels are outside the turning radius
	// Understeer - when front wheels are more angled than turning radius
	// Neutral    - both front rear   are tracking true

	// start oversteer detect
	float oversteer = 0.0f;
	
	float angle = acos(vehData.Velocity / vehData.Speed)* 180.0f / 3.14159265f;
	if (isnan(angle))
		angle = 0.0;

	if (angle > 10 && vehData.Velocity > 1.0f) {
		oversteer = std::min(1.0f, angle/90.0f);
	}
	// end oversteer detect

	// begin understeer detect
	float understeer = 0.0f;
	if (abs(vehData.SteeringAngle*std::sqrt(vehData.Velocity)) > abs(vehData.RotationVelocity.z) &&
		vehData.Velocity > 0.1f) {
		understeer = std::min(1.0f, abs(vehData.SteeringAngle*sqrt(vehData.Velocity) - vehData.RotationVelocity.z));
	}
	// end understeer detect

	// On understeering conditions, lower "grippy" feel
	GForce = std::min(1.0f, std::max(0.0f, 1.0f - understeer + oversteer)) * GForce;

	// Simulate caster instability
	//if (vehData.Velocity < -0.1f) {
	//	GForce = -GForce;
	//}

	// Detail feel / suspension compression based
	float compSpeedTotal = 0.0f;
	auto compSpeed = vehData.GetWheelCompressionSpeeds();

	// More than 2 wheels! Trikes should be ok, etc.
	if (ext.GetNumWheels(vehicle) > 2) {
		// left should pull left, right should pull right
		compSpeedTotal = -compSpeed[0] + compSpeed[1];
	}

	// Cancel all effects except dampening
	if (airborne) {
		GForce = 0.0;
		damperForce = settings.DamperMin;
	}

	if (ignoreSpeed) {
		damperForce = settings.DamperMin;
	}

	if (vehData.Class == VehicleData::VehicleClass::Car || vehData.Class == VehicleData::VehicleClass::Quad) {
		if (VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 0, true) && VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 1, true)) {
			GForce = GForce * 0.1;
			damperForce = settings.DamperMin;
		}
	}
	if (vehData.Class == VehicleData::VehicleClass::Bike) {
		if (VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 0, true)) {
			GForce = GForce * 0.1;
			damperForce = settings.DamperMin;
		}
	}

	if (!VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
		damperForce *= 2;
	}

	int totalForce = 
		static_cast<int>(-GForce * 5000 * settings.PhysicsStrength * settings.FFGlobalMult) + // 2G = max force, Koenigsegg One:1 only does 1.7g!
		static_cast<int>(1000.0f * settings.DetailStrength * compSpeedTotal * settings.FFGlobalMult) +
		static_cast<int>(steerSpeed * damperForce * 0.1) +
		0;

	// Soft lock
	if (effSteer > 1.0f) {
		totalForce = static_cast<int>((effSteer - 1.0f) * 100000) + totalForce;
		if (effSteer > 1.1f) {
			totalForce = 10000;
		}
	} else if (effSteer < -1.0f) {
		totalForce = static_cast<int>((-effSteer - 1.0f) * -100000) + totalForce;
		if (effSteer < -1.1f) {
			totalForce = -10000;
		}
	}
	auto ffAxis = controls.WheelControl.StringToAxis(controls.WheelAxes[static_cast<int>(ScriptControls::WheelAxisType::ForceFeedback)]);
	controls.WheelControl.SetConstantForce(controls.SteerGUID, ffAxis, totalForce);

	if (settings.DisplayInfo) {
		showDebugInfoWheel(settings, effSteer, damperForce, steerSpeed, GForce, oversteer, understeer);
	}
}

///////////////////////////////////////////////////////////////////////////////
//                             Misc features
///////////////////////////////////////////////////////////////////////////////

void functionAutoLookback() {
	if (vehData.CurrGear == 0) {
		CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleLookBehind, 1.0f);
	}
}

void functionAutoGear1() {
	if (vehData.Throttle < 0.1f && vehData.Speed < 0.1f && vehData.CurrGear > 1) {
		vehData.LockGear = 1;
	}
}

void functionHillGravity() {
	if (!controls.BrakeVal
		&& vehData.Speed < 2.0f &&
		VEHICLE::IS_VEHICLE_ON_ALL_WHEELS(vehicle)) {
		float clutchNeutral = vehData.SimulatedNeutral ? 1.0f : controls.ClutchVal;
		if (vehData.Pitch < 0 || clutchNeutral) {
			ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(
				vehicle, 1, 0.0f, -1 * (vehData.Pitch / 150.0f) * 1.1f * clutchNeutral, 0.0f, true, true, true, true);
		}
		if (vehData.Pitch > 10.0f || clutchNeutral)
			ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(
			vehicle, 1, 0.0f, -1 * (vehData.Pitch / 90.0f) * 0.35f * clutchNeutral, 0.0f, true, true, true, true);
	}
}

///////////////////////////////////////////////////////////////////////////////
//                              Script entry
///////////////////////////////////////////////////////////////////////////////

enum eDecorType
{
	DECOR_TYPE_FLOAT = 1,
	DECOR_TYPE_BOOL,
	DECOR_TYPE_INT,
	DECOR_TYPE_UNK,
	DECOR_TYPE_TIME
};

void registerDecorator(const char *thing, eDecorType type) {
	std::string strType = "?????";
	switch (type) {
	case DECOR_TYPE_FLOAT: strType = "float"; break;
	case DECOR_TYPE_BOOL: strType = "bool"; break;
	case DECOR_TYPE_INT: strType = "int"; break;
	case DECOR_TYPE_UNK: strType = "unknown"; break;
	case DECOR_TYPE_TIME: strType = "time"; break;
	}

	if (!DECORATOR::DECOR_IS_REGISTERED_AS_TYPE((char*)thing, type)) {
		DECORATOR::DECOR_REGISTER((char*)thing, type);
		logger.Write("DECOR: Registered \"" + std::string(thing) + "\" as " + strType);
	}
}

// @Unknown Modder
BYTE* g_bIsDecorRegisterLockedPtr = nullptr;
bool setupGlobals() {
	auto addr = mem::FindPattern("\x40\x53\x48\x83\xEC\x20\x80\x3D\x00\x00\x00\x00\x00\x8B\xDA\x75\x29",
								 "xxxxxxxx????xxxxx");
	if (!addr)
		return false;

	g_bIsDecorRegisterLockedPtr = (BYTE*)(addr + *(int*)(addr + 8) + 13);
	*g_bIsDecorRegisterLockedPtr = 0;

	// Legacy support until I get LeFix / XMOD to update
	registerDecorator("doe_elk", DECOR_TYPE_INT);
	registerDecorator("hunt_score", DECOR_TYPE_INT);
	registerDecorator("hunt_weapon", DECOR_TYPE_INT);
	registerDecorator("hunt_chal_weapon", DECOR_TYPE_INT);

	// New decorators! :)
	registerDecorator("mt_gear", DECOR_TYPE_INT);
	registerDecorator("mt_shift_indicator", DECOR_TYPE_INT);
	registerDecorator("mt_neutral", DECOR_TYPE_INT);
	registerDecorator("mt_set_shiftmode", DECOR_TYPE_INT);
	registerDecorator("mt_get_shiftmode", DECOR_TYPE_INT);

	*g_bIsDecorRegisterLockedPtr = 1;
	return true;
}

void main() {
	logger.Write("Script started");

	logger.Write("Setting up globals");
	if (!setupGlobals()) {
		logger.Write("Global setup failed!");
	}

	if (!controls.WheelControl.PreInit()) {
		logger.Write("WHEEL: DirectInput failed to initialize");
	}

	//if (!controls.StickControl.PreInit()) {
	//	logger.Write("STICK: DirectInput failed to initialize");
	//}

	std::string absoluteModPath = Paths::GetModuleFolder(Paths::GetOurModuleHandle()) + mtDir;
	settingsGeneralFile = absoluteModPath + "\\settings_general.ini";
	settingsWheelFile = absoluteModPath + "\\settings_wheel.ini";
	settingsStickFile = absoluteModPath + "\\settings_stick.ini";
	settingsMenuFile = absoluteModPath + "\\settings_menu.ini";
	textureWheelFile = absoluteModPath + "\\texture_wheel.png";

	if (FileExists(textureWheelFile)) {
		textureWheelId = createTexture(textureWheelFile.c_str());
	}
	else {
		logger.Write("ERROR: " + textureWheelFile + " does not exist.");
		textureWheelId = -1;
	}

	settings.SetFiles(settingsGeneralFile, settingsWheelFile, settingsStickFile);


	menu.RegisterOnMain(std::bind(menuInit));
	menu.RegisterOnExit(std::bind(menuClose));
	menu.SetFiles(settingsMenuFile);

	initialize();
	logger.Write("Initialization finished");

	while (true) {
		update();
		update_menu();
		WAIT(0);
	}
}

void ScriptMain() {
	srand(GetTickCount());
	main();
}
