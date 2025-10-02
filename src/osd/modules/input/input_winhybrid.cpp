// license:BSD-3-Clause
// copyright-holders:Brad Hughes, Janko Stamenović
//============================================================
//
//  input_winhybrid.cpp - Windows hybrid DirectInput/Xinput
//
//============================================================

#include "modules/osdmodule.h"

#if defined(OSD_WINDOWS) || defined(SDLMAME_WIN32)

#include "input_dinput.h"
#include "input_xinput.h"

#include <vector>

#include <oleauto.h>
#include <cfgmgr32.h>


namespace osd {

namespace {

struct bstr_deleter
{
	void operator () (BSTR bstr) const
	{
		if (bstr)
			SysFreeString(bstr);
	}
};


typedef std::unique_ptr<OLECHAR, bstr_deleter> bstr_ptr;


//============================================================
//  winhybrid_joystick_module
//============================================================

class winhybrid_joystick_module : public input_module_impl<device_info, osd_common_t>
{
private:
	std::unique_ptr<xinput_api_helper> m_xinput_helper;
	std::unique_ptr<dinput_api_helper> m_dinput_helper;

public:
	winhybrid_joystick_module() :
		input_module_impl<device_info, osd_common_t>(OSD_JOYSTICKINPUT_PROVIDER, "winhybrid")
	{
	}

	virtual bool probe() override
	{
		int status = init_helpers();
		if (status != 0)
		{
			osd_printf_verbose("Hybrid joystick module isn't supported, falling back.\n");
			return false;
		}

		return true;
	}

	virtual int init(osd_interface &osd, const osd_options &options) override
	{
		int status;

		// Call the base
		status = input_module_impl<device_info, osd_common_t>::init(osd, options);
		if (status != 0)
			return status;

		// Create and initialize our helpers
		status = init_helpers();
		if (status != 0)
		{
			osd_printf_error("Hybrid joystick module helpers failed to initialize. Error 0x%X\n", static_cast<unsigned int>(status));
			return status;
		}

		return 0;
	}

	virtual void input_init(running_machine &machine) override
	{
		input_module_impl<device_info, osd_common_t>::input_init(machine);

		bool xinput_detect_failed = false;
		std::vector<DWORD> xinput_deviceids;
		CONFIGRET cret = get_xinput_devices(xinput_deviceids);
		if (cret != CR_SUCCESS)
		{
			xinput_detect_failed = true;
			xinput_deviceids.clear();
			osd_printf_warning("XInput device detection failed. XInput won't be used. Error: 0x%X\n", uint32_t(cret));
		}

		// Enumerate all the DirectInput joysticks and add them if they aren't XInput compatible
		HRESULT result = m_dinput_helper->enum_attached_devices(
				DI8DEVCLASS_GAMECTRL,
				[this, &xinput_deviceids] (LPCDIDEVICEINSTANCE instance)
				{
					// First check if this device is XInput compatible.
					// If so, don't add it here as it'll be picked up by Xinput.
					auto const found = std::find(
							xinput_deviceids.begin(),
							xinput_deviceids.end(),
							instance->guidProduct.Data1);
					if (xinput_deviceids.end() != found)
					{
						osd_printf_verbose("Skipping DirectInput for XInput compatible joystick %S.\n", instance->tszInstanceName);
						return DIENUM_CONTINUE;
					}

					// allocate and link in a new device
					auto devinfo = m_dinput_helper->create_device<dinput_joystick_device>(
							*this,
							instance,
							&c_dfDIJoystick,
							nullptr,
							background_input() ? dinput_cooperative_level::BACKGROUND : dinput_cooperative_level::FOREGROUND,
							[] (auto const &device, auto const &format) -> bool
							{
								// set absolute mode
								HRESULT const result = dinput_api_helper::set_dword_property(
										device,
										DIPROP_AXISMODE,
										0,
										DIPH_DEVICE,
										DIPROPAXISMODE_ABS);
								if ((result != DI_OK) && (result != DI_PROPNOEFFECT))
								{
									osd_printf_error("DirectInput: Unable to set absolute mode for joystick.\n");
									return false;
								}
								return true;
							});
					if (devinfo)
						add_device(DEVICE_CLASS_JOYSTICK, std::move(devinfo));

					return DIENUM_CONTINUE;
				});
		if (result != DI_OK)
			fatalerror("DirectInput: Unable to enumerate game controllers (result=%08X).\n", uint32_t(result));

		// now add all xinput devices
		if (!xinput_detect_failed)
		{
			// Loop through each gamepad to determine if they are connected
			for (UINT i = 0; i < XUSER_MAX_COUNT; i++)
			{
				// allocate and link in a new device
				auto devinfo = m_xinput_helper->create_xinput_device(i, *this);
				if (devinfo)
					add_device(DEVICE_CLASS_JOYSTICK, std::move(devinfo));
			}
		}
	}

	virtual void exit() override
	{
		input_module_impl<device_info, osd_common_t>::exit();

		m_xinput_helper.reset();
		m_dinput_helper.reset();
	}

private:
	int init_helpers()
	{
		if (!m_xinput_helper)
		{
			m_xinput_helper = std::make_unique<xinput_api_helper>();
			int const status = m_xinput_helper->initialize();
			if (status != 0)
			{
				osd_printf_verbose("Failed to initialize XInput API! Error: %u\n", static_cast<unsigned int>(status));
				return -1;
			}
		}

		if (!m_dinput_helper)
		{
			m_dinput_helper = std::make_unique<dinput_api_helper>();
			int const status = m_dinput_helper->initialize();
			if (status != DI_OK)
			{
				osd_printf_verbose("Failed to initialize DirectInput API! Error: %u\n", static_cast<unsigned int>(status));
				return -1;
			}
		}

		return 0;
	}

	BOOL get_4hexd_id(PCSTR const strIn, PCSTR const prefix, PCSTR const fmt, DWORD* pval)
	{
		PCSTR const strFound = strstr(strIn, prefix);
		return strFound && sscanf(strFound, fmt, pval) == 1;
	}

	//-----------------------------------------------------------------------------
	// Enum each present PNP device in device information set using cfgmgr32
	// and check each device ID to see if it contains
	// "IG_" (ex. "VID_045E&PID_028E&IG_00").  If it does, then it's an XInput device
	// Unfortunately this information can not be found by just using DirectInput.
	// Checking against a VID/PID of 0x028E/0x045E won't find 3rd party or future
	// XInput devices.
	//-----------------------------------------------------------------------------
	CONFIGRET get_xinput_devices(std::vector<DWORD> &xinput_deviceids)
	{
		ULONG cbBuff = 0;
		ULONG flags = CM_GETIDLIST_FILTER_PRESENT;
		CONFIGRET retval = CM_Get_Device_ID_List_SizeA(&cbBuff, nullptr, flags);
		if (retval != CR_SUCCESS) {
			osd_printf_error("CM_Get_Device_ID_List_SizeA failed.\n");
			return retval;
		}
		std::vector<char> buff;
		buff.resize(cbBuff);
		retval = CM_Get_Device_ID_ListA(nullptr, buff.data(), cbBuff, flags);
		if (retval != CR_SUCCESS) {
			osd_printf_error("CM_Get_Device_ID_ListA failed.\n");
			return retval;
		}
		char* strDeviceID = buff.data();
		DWORD len = strlen(strDeviceID);
		while (len) 
		{
			printf( "%s\n", strDeviceID );
			// Check if the device ID contains "IG_".  If it does, then it's an XInput device
			// Unfortunately this information can not be found by just using DirectInput
			// If it does, then get the VID/PID
			DWORD dwVid = 0;
			DWORD dwPid = 0;
			if (strstr(strDeviceID, "IG_") &&
				get_4hexd_id(strDeviceID, "VID_", "VID_%4X", &dwVid) &&
				get_4hexd_id(strDeviceID, "PID_", "PID_%4X", &dwPid))
			{
				// Add the VID/PID to a list
				xinput_deviceids.push_back(MAKELONG(dwVid, dwPid));
			}
			strDeviceID += len + 1;
			len = strlen(strDeviceID);
		}
		return CR_SUCCESS;
	}
};

} // anonymous namespace

} // namespace osd

#else // defined(OSD_WINDOWS) || defined(SDLMAME_WIN32)

#include "input_module.h"

namespace osd { namespace { MODULE_NOT_SUPPORTED(winhybrid_joystick_module, OSD_JOYSTICKINPUT_PROVIDER, "winhybrid") } }

#endif // defined(OSD_WINDOWS) || defined(SDLMAME_WIN32)

MODULE_DEFINITION(JOYSTICKINPUT_WINHYBRID, osd::winhybrid_joystick_module)
