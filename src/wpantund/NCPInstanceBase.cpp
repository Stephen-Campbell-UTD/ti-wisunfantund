/*
 *
 * Copyright (c) 2016 Nest Labs, Inc.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by Texas Instruments - 2021
 *
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif

#include "assert-macros.h"
#include "NCPInstanceBase.h"
#include <iostream>
#include "tunnel.h"
#include <syslog.h>
#include <errno.h>
#include "nlpt.h"
#include <algorithm>
#include "socket-utils.h"
#include "SuperSocket.h"
#include "wpantund.h"
#include "any-to.h"
#include "IPv6Helpers.h"
#include <math.h>
#include <vector>

using namespace nl;
using namespace wpantund;

// ----------------------------------------------------------------------------
// MARK: -
// MARK: Constructors/Destructors

NCPInstanceBase::NCPInstanceBase(const Settings& settings):
	mCommissioningRule(),
	mCommissioningExpiration(0)
{
	std::string wpan_interface_name = "wfan0";

	mResetSocket_BeginReset = '0';
	mResetSocket_EndReset = '1';

	mPowerSocket_PowerOff = '0';
	mPowerSocket_PowerOn = '1';

	NLPT_INIT(&mNCPToDriverPumpPT);
	NLPT_INIT(&mDriverToNCPPumpPT);

	mAutoDeepSleep = false;
	mAutoDeepSleepTimeout = 10;
	mAutoResume = true;
	mAutoUpdateFirmware = false;
	mCommissionerPort = 5684;
	mCommissioningExpiration = 0;
	mEnabled = true;
	mFailureCount = 0;
	mFailureThreshold = 3;
	mIsInitializingNCP = false;
	mIsInterfaceOnline = false;
	mLastChangedBusy = 0;
	mLegacyInterfaceEnabled = false;
	mNCPState = UNINITIALIZED;
	mRequestRouteRefresh = false;
	mNodeType = UNKNOWN;
	mNodeTypeSupportsLegacy = false;
	mAutoUpdateInterfaceIPv6AddrsOnNCP = true;
	mFilterUserAddedLinkLocalIPv6Address = true;
	mAutoAddSLAACAddress = true;
	mNCPHandlesSLAAC = false;
	mSetDefaultRouteForAutoAddedPrefix = false;
	mSetSLAACForAutoAddedPrefix = false;
	mAutoAddOffMeshRoutesOnInterface = true;
	mFilterSelfAutoAddedOffMeshRoutes = true;
	mAutoAddOnMeshPrefixesAsInterfaceRoutes = true;
	mTerminateOnFault = false;
	mWasBusy = false;
	mNCPIsMisbehaving = false;

	/* Initialize mMacFilterListString */
	std::string ret_string = "";
	int string_count = 0;

	for (int x = 0; x < (MAC_FILTER_LIST_SIZE * 2); x += 2){
		char str_to_add[17];
		sprintf(str_to_add, "%08x", mMacFilterList[x]);
		ret_string = str_to_add;
		sprintf(str_to_add, "%08x", mMacFilterList[x + 1]);
		ret_string.append(str_to_add);
		mMacFilterListString[string_count] = ret_string;
		string_count ++;
	}

	regsiter_all_get_handlers();
	regsiter_all_set_handlers();
	regsiter_all_insert_handlers();
	regsiter_all_remove_handlers();

	memset(mNCPMeshLocalAddress.s6_addr, 0, sizeof(mNCPMeshLocalAddress));
	memset(mNCPLinkLocalAddress.s6_addr, 0, sizeof(mNCPLinkLocalAddress));
	memset(mNCPV6LegacyPrefix, 0, sizeof(mNCPV6LegacyPrefix));
	memset(mMACAddress, 0, sizeof(mMACAddress));
	memset(mMACHardwareAddress, 0, sizeof(mMACHardwareAddress));

	if (!settings.empty()) {
		Settings::const_iterator iter;

		for(iter = settings.begin(); iter != settings.end(); iter++) {
			if (strcaseequal(iter->first.c_str(), kWPANTUNDProperty_ConfigNCPHardResetPath)) {
				mResetSocket = SuperSocket::create(iter->second);

			} else if (strcaseequal(iter->first.c_str(), kWPANTUNDProperty_ConfigNCPPowerPath)) {
				mPowerSocket = SuperSocket::create(iter->second);

			} else if (strcaseequal(iter->first.c_str(), kWPANTUNDProperty_ConfigNCPSocketPath)) {
				mRawSerialAdapter = SuperSocket::create(iter->second);

			} else if (strcaseequal(iter->first.c_str(), kWPANTUNDProperty_ConfigTUNInterfaceName)) {
				wpan_interface_name = iter->second;

			} else if (strcaseequal(iter->first.c_str(), kWPANTUNDProperty_ConfigNCPFirmwareCheckCommand)) {
				mFirmwareUpgrade.set_firmware_check_command(iter->second);

			} else if (strcaseequal(iter->first.c_str(), kWPANTUNDProperty_ConfigNCPFirmwareUpgradeCommand)) {
				mFirmwareUpgrade.set_firmware_upgrade_command(iter->second);

			} else if (strcaseequal(iter->first.c_str(), kWPANTUNDProperty_DaemonAutoFirmwareUpdate)) {
				mAutoUpdateFirmware = any_to_bool(boost::any(iter->second));

			} else if (strcaseequal(iter->first.c_str(), kWPANTUNDProperty_ConfigDaemonNetworkRetainCommand)) {
				mNetworkRetain.set_network_retain_command(iter->second);
			}
		}
	}

	if (!mRawSerialAdapter) {
		syslog(LOG_WARNING, kWPANTUNDProperty_ConfigNCPSocketPath " was not specified. Using \"/dev/null\" instead.");
		mRawSerialAdapter = SuperSocket::create("/dev/null");
	}

	if (mRawSerialAdapter) {
		mRawSerialAdapter->set_log_level(LOG_DEBUG);
	}

	mSerialAdapter = mRawSerialAdapter;

	mPrimaryInterface = boost::shared_ptr<TunnelIPv6Interface>(new TunnelIPv6Interface(wpan_interface_name));
	mPrimaryInterface->mUnicastAddressWasAdded.connect(boost::bind(&NCPInstanceBase::unicast_address_was_added, this, kOriginPrimaryInterface, _1, _2, UINT32_MAX, UINT32_MAX));
	mPrimaryInterface->mUnicastAddressWasRemoved.connect(boost::bind(&NCPInstanceBase::unicast_address_was_removed, this, kOriginPrimaryInterface, _1));
	mPrimaryInterface->mMulticastAddressWasJoined.connect(boost::bind(&NCPInstanceBase::multicast_address_was_joined, this, kOriginPrimaryInterface, _1, NilReturn()));
	mPrimaryInterface->mMulticastAddressWasLeft.connect(boost::bind(&NCPInstanceBase::multicast_address_was_left, this, kOriginPrimaryInterface, _1, NilReturn()));

	mPrimaryInterface->mLinkStateChanged.connect(boost::bind(&NCPInstanceBase::link_state_changed, this, _1, _2));

	set_ncp_power(true);

	// Go ahead and start listening on ff03::1
	join_multicast_group("ff03::1");

	{
		IPv6PacketMatcherRule rule;

		// --------------------------------------------------------------------
		// Packet Drop rules

		rule.clear();
		// OS X seems to generate these packets when bringing up the interface.
		// Honey badger don't care.
		rule.type = IPv6PacketMatcherRule::TYPE_HOP_BY_HOP;
		rule.remote_address.s6_addr[0x0] = 0xFF;
		rule.remote_address.s6_addr[0x1] = 0x02;
		rule.remote_address.s6_addr[0xF] = 0x16;
		rule.remote_match_mask = 128;
		mDropFirewall.insert(rule);

		rule.clear();
		// Don't forward router advertisement or router solicitation
		// traffic.
		rule.type = IPv6PacketMatcherRule::TYPE_ICMP;
		rule.remote_address.s6_addr[0x0] = 0xFF;
		rule.remote_address.s6_addr[0x1] = 0x02;
		rule.remote_address.s6_addr[0xF] = 0x02;
		rule.remote_match_mask = 128;
		rule.subtype = IPv6PacketMatcherRule::SUBTYPE_ICMP_ROUTER_ADV;
		mDropFirewall.insert(rule);
		rule.subtype = IPv6PacketMatcherRule::SUBTYPE_ICMP_ROUTER_SOL;
		mDropFirewall.insert(rule);

		rule.clear();
		// Don't forward neighbor advertisement or neighbor solicitation
		// or redirect traffic.
		rule.type = IPv6PacketMatcherRule::TYPE_ICMP;
		rule.subtype = IPv6PacketMatcherRule::SUBTYPE_ICMP_NEIGHBOR_ADV;
		mDropFirewall.insert(rule);
		rule.subtype = IPv6PacketMatcherRule::SUBTYPE_ICMP_NEIGHBOR_SOL;
		mDropFirewall.insert(rule);
		rule.subtype = IPv6PacketMatcherRule::SUBTYPE_ICMP_REDIRECT;
		mDropFirewall.insert(rule);
	}
}

bool
NCPInstanceBase::setup_property_supported_by_class(const std::string& prop_name)
{
	return strcaseequal(prop_name.c_str(), kWPANTUNDProperty_ConfigNCPHardResetPath)
		|| strcaseequal(prop_name.c_str(), kWPANTUNDProperty_ConfigNCPPowerPath)
		|| strcaseequal(prop_name.c_str(), kWPANTUNDProperty_ConfigNCPSocketPath)
		|| strcaseequal(prop_name.c_str(), kWPANTUNDProperty_ConfigTUNInterfaceName)
		|| strcaseequal(prop_name.c_str(), kWPANTUNDProperty_ConfigNCPDriverName)
		|| strcaseequal(prop_name.c_str(), kWPANTUNDProperty_ConfigNCPFirmwareCheckCommand)
		|| strcaseequal(prop_name.c_str(), kWPANTUNDProperty_DaemonAutoFirmwareUpdate)
		|| strcaseequal(prop_name.c_str(), kWPANTUNDProperty_ConfigNCPFirmwareUpgradeCommand)
		|| strcaseequal(prop_name.c_str(), kWPANTUNDProperty_ConfigDaemonNetworkRetainCommand);
}

NCPInstanceBase::~NCPInstanceBase()
{
}

const std::string &
NCPInstanceBase::get_name(void)
{
	return mPrimaryInterface->get_interface_name();
}

const WPAN::NetworkInstance&
NCPInstanceBase::get_current_network_instance(void) const
{
	return mCurrentNetworkInstance;
}

// Helpful for use with callbacks.
int
NCPInstanceBase::process_event_helper(int event)
{
	return EventHandler::process_event(event);
}

// ----------------------------------------------------------------------------
// MARK: -

void NCPInstanceBase::add_service(uint32_t enterprise_number,
	const Data &service_data, bool stable, const Data &server_data,
	CallbackWithStatus cb)
{
	add_service_on_ncp(enterprise_number, service_data, stable, server_data, cb);
}

void NCPInstanceBase::remove_service(uint32_t enterprise_number,
	const Data &service_data, CallbackWithStatus cb)
{
	remove_service_on_ncp(enterprise_number, service_data, cb);
}

wpantund_status_t
NCPInstanceBase::set_ncp_version_string(const std::string& version_string)
{
	wpantund_status_t status = kWPANTUNDStatus_Ok;

	if (version_string != mNCPVersionString) {
		if (!mNCPVersionString.empty()) {
			// The previous version string isn't empty!
			syslog(LOG_ERR, "Illegal NCP version change! (Previously \"%s\")", mNCPVersionString.c_str());
			ncp_is_misbehaving();
			status = kWPANTUNDStatus_InvalidArgument;
		} else {
			mNCPVersionString = version_string;

			syslog(LOG_NOTICE, "NCP is running \"%s\"", mNCPVersionString.c_str());
			syslog(LOG_NOTICE, "Driver is running \"%s\"", nl::wpantund::get_wpantund_version_string().c_str());

			if (mAutoUpdateFirmware && is_firmware_upgrade_required(version_string)) {
				syslog(LOG_NOTICE, "NCP FIRMWARE UPGRADE IS REQUIRED");
				upgrade_firmware();
			}
		}
	}
	return status;
}

void NCPInstanceBase::set_ncp_region(uint8_t region){
	mNCPRegion = region;
}
void NCPInstanceBase::set_ncp_mode_id(int mode_id){
	mNCPModeID = mode_id;
}
void NCPInstanceBase::set_ncp_protocol_version(const int protocol_version_major, const int protocol_version_minor){
	mNCPProtocolVersionMajor = protocol_version_major;
	mNCPProtocolVersionMinor = protocol_version_minor;
}
void NCPInstanceBase::set_ncp_interface_type(const int interface_type){
	mNCPInterfaceTypeInt = interface_type;
}
void NCPInstanceBase::set_ncp_cca_threshold(const int cca_threshold){
	mNCPCCAThresholdInt = cca_threshold;
}
void NCPInstanceBase::set_stack_up(const bool stack_up){
	mStackUp = stack_up;
}
void NCPInstanceBase::set_if_up(const bool if_up){
	mIfUp = if_up;
}
void NCPInstanceBase::set_connected_devices(const int connected_devices){
	mConnectedDevices = connected_devices;
}
void NCPInstanceBase::set_num_connected_devices(const int connected_devices){
	mNumConnectedDevices = connected_devices;
}
void NCPInstanceBase::set_ch0_center_freq(const int ch0_mhz, const int ch0_khz){
	mCh0khz = ch0_khz;
	mCh0mhz = ch0_mhz;
}
void NCPInstanceBase::set_ch_spacing(const int ch_spacing){
	mChSpacing = ch_spacing;
}
void NCPInstanceBase::set_bc_interval(const int bc_interval){
	mBCInterval = bc_interval;
}
void NCPInstanceBase::set_mac_filter_list(int filter_list[]){
	for (int x = 0; x < MAC_FILTER_LIST_SIZE * 2; x++){
		mMacFilterList[x] = filter_list[x];
	}
}
void NCPInstanceBase::set_mac_filter_list_string(std::string filter_list[]){
	for (int x = 0; x < MAC_FILTER_LIST_SIZE; x++){
		mMacFilterListString[x] = filter_list[x];
	}
}
void NCPInstanceBase::set_mac_filter_mode(const int filter_mode){
	mMacFilterMode = filter_mode;
}
void NCPInstanceBase::set_uc_dwell_interval(const int uc_dwell_interval){
	mUCDwellInterval = uc_dwell_interval;
}
void NCPInstanceBase::set_bc_dwell_interval(const int bc_dwell_interval){
	mBCDwellInterval = bc_dwell_interval;
}
void NCPInstanceBase::set_uc_channel_function(const int uc_channel_function){
	mUCChFunction = uc_channel_function;
}
void NCPInstanceBase::set_bc_channel_function(const int bc_channel_function){
	mBCChFunction = bc_channel_function;
}
void NCPInstanceBase::set_unicast_channel_list(std::string unicast_channel_list){
	mUnicastChList = unicast_channel_list;
}
void NCPInstanceBase::set_unicast_array(int unicast_array[]){
	for (int x = 0; x < CHANNEL_LIST_SIZE; x++){
		mUnicastBytes[x] = unicast_array[x];
	}
}
void NCPInstanceBase::set_broadcast_channel_list(std::string broadcast_channel_list){
	mBroadcastChList = broadcast_channel_list;
}
void NCPInstanceBase::set_broadcast_array(int broadcast_array[]){
	for (int x = 0; x < CHANNEL_LIST_SIZE; x++){
		mBroadcastBytes[x] = broadcast_array[x];
	}
}
void NCPInstanceBase::set_async_channel_list(std::string async_channel_list){
	mAsyncChList = async_channel_list;
}
void NCPInstanceBase::set_async_array(int async_array[]){
	for (int x = 0; x < CHANNEL_LIST_SIZE; x++){
		mAsyncBytes[x] = async_array[x];
	}
}
void NCPInstanceBase::set_dodag_route_string(std::string dodag_route_dest_string){
	mDodagRouteDest = dodag_route_dest_string;
}
void NCPInstanceBase::set_dodag_route_array(int dodag_route_dest_array[]){
	for (int x = 0; x < DODAG_ROUTE_SIZE; x++){
		mDodagRouteDestArray[x] = dodag_route_dest_array[x];
	}
}

std::set<std::string>
NCPInstanceBase::get_supported_property_keys(void) const
{
	std::set<std::string> properties;
	properties.insert(kWPANTUNDProperty_NCPProtocolVersion);
	properties.insert(kWPANTUNDProperty_NCPVersion);
	properties.insert(kWPANTUNDProperty_NCPInterfaceType);
	properties.insert(kWPANTUNDProperty_NCPHardwareAddress);
	properties.insert(kWPANTUNDProperty_NCPCCAThreshold);
	properties.insert(kWPANTUNDProperty_NCPTXPower);
	properties.insert(kWPANTUNDProperty_NCPPhyRegion);
	properties.insert(kWPANTUNDProperty_UnicastChList);
	properties.insert(kWPANTUNDProperty_BroadcastChList);
	properties.insert(kWPANTUNDProperty_AsyncChList);
	properties.insert(kWPANTUNDProperty_ChSpacing);
	properties.insert(kWPANTUNDProperty_Ch0CenterFreq);
	properties.insert(kWPANTUNDProperty_NetworkPANID);
	properties.insert(kWPANTUNDProperty_UCDwellInterval);
	properties.insert(kWPANTUNDProperty_BCDwellInterval);
	properties.insert(kWPANTUNDProperty_BCInterval);
	properties.insert(kWPANTUNDProperty_UCChFunction);
	properties.insert(kWPANTUNDProperty_BCChFunction);
	properties.insert(kWPANTUNDProperty_MacFilterList);
	properties.insert(kWPANTUNDProperty_MacFilterMode);
	properties.insert(kWPANTUNDProperty_InterfaceUp);
	properties.insert(kWPANTUNDProperty_StackUp);
	properties.insert(kWPANTUNDProperty_NetworkRole);
	properties.insert(kWPANTUNDProperty_NetworkName);
	properties.insert(kWPANTUNDProperty_DodagRouteDest);
	properties.insert(kWPANTUNDProperty_DodagRoute);
	properties.insert(kWPANTUNDProperty_NumConnectedDevices);
	properties.insert(kWPANTUNDProperty_ConnectedDevices);
	properties.insert(kWPANTUNDProperty_IPv6AllAddresses);

	return properties;
}

std::string
NCPInstanceBase::to_upper(const std::string &str)
{
	std::string new_str = str;

	for (size_t i = 0; i < str.length(); i++) {
		new_str[i] = std::toupper(new_str[i]);
	}

	return new_str;
}

// ----------------------------------------------------------------------------
// MARK: -
// MARK: Property Get Handlers

void
NCPInstanceBase::register_prop_get_handler(const char *prop, PropGetHandler handler)
{
	std::string key = to_upper(prop);
	mPropertyGetHandlers[key] = PropGetHandlerEntry(prop, handler);
}

void
NCPInstanceBase::regsiter_all_get_handlers(void)
{
#define REGISTER_GET_HANDLER(name)     \
	register_prop_get_handler(kWPANTUNDProperty_##name, boost::bind(&NCPInstanceBase::get_prop_##name, this, _1))

	register_prop_get_handler("", boost::bind(&NCPInstanceBase::get_prop_empty, this, _1));

	REGISTER_GET_HANDLER(DodagRouteDest);
	REGISTER_GET_HANDLER(NCPPhyRegion);
	REGISTER_GET_HANDLER(NCPModeID);
	REGISTER_GET_HANDLER(NCPProtocolVersion);
	REGISTER_GET_HANDLER(NCPVersion);
	REGISTER_GET_HANDLER(NCPInterfaceType);
	REGISTER_GET_HANDLER(NCPHardwareAddress);
	REGISTER_GET_HANDLER(NCPCCAThreshold);
	REGISTER_GET_HANDLER(NCPTXPower);
	REGISTER_GET_HANDLER(NCPFrequency);
	REGISTER_GET_HANDLER(NetworkPANID);
	REGISTER_GET_HANDLER(StackUp);
	REGISTER_GET_HANDLER(ConnectedDevices);
	REGISTER_GET_HANDLER(NumConnectedDevices);
	REGISTER_GET_HANDLER(Ch0CenterFreq);
	REGISTER_GET_HANDLER(ChSpacing);
	REGISTER_GET_HANDLER(BCInterval);
	REGISTER_GET_HANDLER(MacFilterList);
	REGISTER_GET_HANDLER(MacFilterMode);
	REGISTER_GET_HANDLER(UCDwellInterval);
	REGISTER_GET_HANDLER(BCDwellInterval);
	REGISTER_GET_HANDLER(UCChFunction);
	REGISTER_GET_HANDLER(BCChFunction);
	REGISTER_GET_HANDLER(UnicastChList);
	REGISTER_GET_HANDLER(BroadcastChList);
	REGISTER_GET_HANDLER(AsyncChList);
	REGISTER_GET_HANDLER(ConfigTUNInterfaceName);
	REGISTER_GET_HANDLER(DaemonEnabled);
	REGISTER_GET_HANDLER(InterfaceUp);
	REGISTER_GET_HANDLER(DaemonReadyForHostSleep);
	REGISTER_GET_HANDLER(NCPVersion);
	REGISTER_GET_HANDLER(NetworkName);
	REGISTER_GET_HANDLER(NetworkIsCommissioned);
	REGISTER_GET_HANDLER(NestLabs_LegacyEnabled);
	REGISTER_GET_HANDLER(NestLabs_NetworkAllowingJoin);
	REGISTER_GET_HANDLER(NetworkXPANID);
	REGISTER_GET_HANDLER(NCPChannel);
	REGISTER_GET_HANDLER(DaemonVersion);
	REGISTER_GET_HANDLER(DaemonAutoAssociateAfterReset);
	REGISTER_GET_HANDLER(DaemonAutoDeepSleep);
	REGISTER_GET_HANDLER(DaemonAutoFirmwareUpdate);
	REGISTER_GET_HANDLER(DaemonTerminateOnFault);
	REGISTER_GET_HANDLER(DaemonIPv6AutoUpdateIntfaceAddrOnNCP);
	REGISTER_GET_HANDLER(DaemonIPv6FilterUserAddedLinkLocal);
	REGISTER_GET_HANDLER(DaemonIPv6AutoAddSLAACAddress);
	REGISTER_GET_HANDLER(DaemonSetDefRouteForAutoAddedPrefix);
	REGISTER_GET_HANDLER(NestLabs_NetworkPassthruPort);
	REGISTER_GET_HANDLER(NCPMACAddress);
	REGISTER_GET_HANDLER(NCPHardwareAddress);
	REGISTER_GET_HANDLER(IPv6SetSLAACForAutoAddedPrefix);
	REGISTER_GET_HANDLER(DaemonOffMeshRouteAutoAddOnInterface);
	REGISTER_GET_HANDLER(DaemonOffMeshRouteFilterSelfAutoAdded);
	REGISTER_GET_HANDLER(DaemonOnMeshPrefixAutoAddAsIfaceRoute);
	REGISTER_GET_HANDLER(IPv6MeshLocalPrefix);
	REGISTER_GET_HANDLER(IPv6LinkLocalAddress);
	REGISTER_GET_HANDLER(NestLabs_LegacyMeshLocalPrefix);
	REGISTER_GET_HANDLER(NestLabs_LegacyMeshLocalAddress);
	REGISTER_GET_HANDLER(NCPState);
	REGISTER_GET_HANDLER(NetworkNodeType);
	REGISTER_GET_HANDLER(ThreadOnMeshPrefixes);
	REGISTER_GET_HANDLER(ThreadOffMeshRoutes);
	REGISTER_GET_HANDLER(ThreadServices);
	REGISTER_GET_HANDLER(ThreadServicesAsValMap);
	REGISTER_GET_HANDLER(IPv6AllAddresses);
	REGISTER_GET_HANDLER(IPv6MulticastAddresses);
	REGISTER_GET_HANDLER(IPv6InterfaceRoutes);
	REGISTER_GET_HANDLER(DaemonSyslogMask);

#undef REGISTER_GET_HANDLER
}

void
NCPInstanceBase::property_get_value(const std::string &key, CallbackWithStatusArg1 cb)
{
	std::map<std::string, PropGetHandlerEntry>::iterator iter;

	iter = mPropertyGetHandlers.find(to_upper(key));

	if (iter != mPropertyGetHandlers.end()) {
		iter->second(cb);

	} else if (StatCollector::is_a_stat_property(key)) {
		get_stat_collector().property_get_value(key, cb);

	} else {
		syslog(LOG_ERR, "property_get_value: Unsupported property \"%s\"", key.c_str());
		cb(kWPANTUNDStatus_PropertyNotFound, boost::any(std::string("Property Not Found")));
	}
}
void
NCPInstanceBase::get_prop_DodagRouteDest(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mDodagRouteDest));
}
void
NCPInstanceBase::get_prop_NCPPhyRegion(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(ncp_region_to_string(mNCPRegion)));
}
void
NCPInstanceBase::get_prop_NCPModeID(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mNCPModeID));
}
void
NCPInstanceBase::get_prop_NCPProtocolVersion(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(ncp_protocol_version_to_string(mNCPProtocolVersionMajor, mNCPProtocolVersionMinor)));
}
void
NCPInstanceBase::get_prop_NCPVersion(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mNCPVersionString));
}
void
NCPInstanceBase::get_prop_NCPInterfaceType(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mNCPInterfaceTypeInt));
}
void
NCPInstanceBase::get_prop_NCPHardwareAddress(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(nl::Data(mMACHardwareAddress, sizeof(mMACHardwareAddress))));
}

void
NCPInstanceBase::get_prop_NCPCCAThreshold(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mNCPCCAThresholdInt));
}
void
NCPInstanceBase::get_prop_NCPTXPower(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mNCPTXPowerInt));
}

void
NCPInstanceBase::get_prop_NCPFrequency(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mNCPFrequencyDouble));
}

void
NCPInstanceBase::get_prop_NetworkPANID(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(get_current_network_instance().panid));
}

void
NCPInstanceBase::get_prop_StackUp(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mStackUp));
}

void
NCPInstanceBase::get_prop_ConnectedDevices(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mNumConnectedDevices));
}

void
NCPInstanceBase::get_prop_NumConnectedDevices(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mNumConnectedDevices));
}

void
NCPInstanceBase::get_prop_Ch0CenterFreq(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(ch0_center_freq_to_string(mCh0mhz, mCh0khz)));
}

void
NCPInstanceBase::get_prop_ChSpacing(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(ch_spacing_to_string(mChSpacing)));
}

void
NCPInstanceBase::get_prop_BCInterval(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mBCInterval));
}

void
NCPInstanceBase::get_prop_MacFilterList(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mac_filter_list_to_string(mMacFilterListString)));
}

void
NCPInstanceBase::get_prop_MacFilterMode(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mMacFilterMode));
}

void
NCPInstanceBase::get_prop_UCDwellInterval(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mUCDwellInterval));
}

void
NCPInstanceBase::get_prop_BCDwellInterval(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mBCDwellInterval));
}

void
NCPInstanceBase::get_prop_UCChFunction(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mUCChFunction));
}

void
NCPInstanceBase::get_prop_BCChFunction(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mBCChFunction));
}

void
NCPInstanceBase::get_prop_UnicastChList(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mUnicastChList));
}

void
NCPInstanceBase::get_prop_BroadcastChList(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mBroadcastChList));
}

void
NCPInstanceBase::get_prop_AsyncChList(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mAsyncChList));
}

void
NCPInstanceBase::get_prop_empty(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, get_supported_property_keys());
}

void
NCPInstanceBase::get_prop_ConfigTUNInterfaceName(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, get_name());
}

void
NCPInstanceBase::get_prop_DaemonEnabled(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mEnabled));
}

void
NCPInstanceBase::get_prop_InterfaceUp(CallbackWithStatusArg1 cb)
{
	//cb(kWPANTUNDStatus_Ok, boost::any(mPrimaryInterface->is_online()));
	cb(kWPANTUNDStatus_Ok, boost::any(mIfUp));
}

void
NCPInstanceBase::get_prop_DaemonReadyForHostSleep(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(!is_busy()));
}

void
NCPInstanceBase::get_prop_NetworkName(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(get_current_network_instance().name));
}

void
NCPInstanceBase::get_prop_NetworkIsCommissioned(CallbackWithStatusArg1 cb)
{
	NCPState ncp_state = get_ncp_state();

	if (ncp_state_is_commissioned(ncp_state)) {
		cb(kWPANTUNDStatus_Ok, boost::any(true));

	} else if (ncp_state == OFFLINE || ncp_state == DEEP_SLEEP) {
		cb(kWPANTUNDStatus_Ok, boost::any(false));

	} else {
		cb(
			kWPANTUNDStatus_TryAgainLater,
			boost::any(std::string("Unable to determine association state at this time"))
		);
	}
}

void
NCPInstanceBase::get_prop_NestLabs_LegacyEnabled(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mLegacyInterfaceEnabled));
}

void
NCPInstanceBase::get_prop_NestLabs_NetworkAllowingJoin(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(get_current_network_instance().joinable));
}


void
NCPInstanceBase::get_prop_NetworkXPANID(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(get_current_network_instance().get_xpanid_as_uint64()));
}

void
NCPInstanceBase::get_prop_NCPChannel(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any((int)get_current_network_instance().channel));
}

void
NCPInstanceBase::get_prop_DaemonVersion(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(nl::wpantund::get_wpantund_version_string()));
}

void
NCPInstanceBase::get_prop_DaemonAutoAssociateAfterReset(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(static_cast<bool>(mAutoResume)));
}

void
NCPInstanceBase::get_prop_DaemonAutoDeepSleep(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mAutoDeepSleep));
}

void
NCPInstanceBase::get_prop_DaemonAutoFirmwareUpdate(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mAutoUpdateFirmware));
}

void
NCPInstanceBase::get_prop_DaemonTerminateOnFault(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mTerminateOnFault));
}

void
NCPInstanceBase::get_prop_DaemonIPv6AutoUpdateIntfaceAddrOnNCP(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mAutoUpdateInterfaceIPv6AddrsOnNCP));
}

void
NCPInstanceBase::get_prop_DaemonIPv6FilterUserAddedLinkLocal(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mFilterUserAddedLinkLocalIPv6Address));
}

void
NCPInstanceBase::get_prop_DaemonIPv6AutoAddSLAACAddress(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mAutoAddSLAACAddress));
}

void
NCPInstanceBase::get_prop_DaemonSetDefRouteForAutoAddedPrefix(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mSetDefaultRouteForAutoAddedPrefix));
}

void
NCPInstanceBase::get_prop_NestLabs_NetworkPassthruPort(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mCommissionerPort));
}

void
NCPInstanceBase::get_prop_NCPMACAddress(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(nl::Data(mMACAddress, sizeof(mMACAddress))));
}

void
NCPInstanceBase::get_prop_IPv6SetSLAACForAutoAddedPrefix(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mSetSLAACForAutoAddedPrefix));
}

void
NCPInstanceBase::get_prop_DaemonOffMeshRouteAutoAddOnInterface(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mAutoAddOffMeshRoutesOnInterface));
}

void
NCPInstanceBase::get_prop_DaemonOffMeshRouteFilterSelfAutoAdded(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mFilterSelfAutoAddedOffMeshRoutes));
}

void
NCPInstanceBase::get_prop_DaemonOnMeshPrefixAutoAddAsIfaceRoute(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(mAutoAddOnMeshPrefixesAsInterfaceRoutes));
}

void
NCPInstanceBase::get_prop_IPv6MeshLocalPrefix(CallbackWithStatusArg1 cb)
{
	if (buffer_is_nonzero(mNCPV6Prefix, sizeof(mNCPV6Prefix))) {
		struct in6_addr addr (mNCPMeshLocalAddress);
		// Zero out the lower 64 bits.
		memset(addr.s6_addr+8, 0, 8);
		cb(kWPANTUNDStatus_Ok, boost::any(in6_addr_to_string(addr)+"/64"));
	} else {
		cb(kWPANTUNDStatus_FeatureNotSupported, std::string("Property is unavailable"));
	}
}

void
NCPInstanceBase::get_prop_IPv6LinkLocalAddress(CallbackWithStatusArg1 cb)
{
	if (buffer_is_nonzero(mNCPLinkLocalAddress.s6_addr, sizeof(mNCPLinkLocalAddress))) {
		cb(kWPANTUNDStatus_Ok, boost::any(in6_addr_to_string(mNCPLinkLocalAddress)));
	} else {
		cb(kWPANTUNDStatus_FeatureNotSupported, std::string("Property is unavailable"));
	}
}

void
NCPInstanceBase::get_prop_NestLabs_LegacyMeshLocalPrefix(CallbackWithStatusArg1 cb)
{
	if (mLegacyInterfaceEnabled
		|| mNodeTypeSupportsLegacy
		|| buffer_is_nonzero(mNCPV6LegacyPrefix, sizeof(mNCPV6LegacyPrefix))
	) {
		cb(kWPANTUNDStatus_Ok, boost::any(nl::Data(mNCPV6LegacyPrefix, sizeof(mNCPV6LegacyPrefix))));
	} else {
		cb(kWPANTUNDStatus_FeatureNotSupported, std::string("Property is unavailable"));
	}
}

void
NCPInstanceBase::get_prop_NestLabs_LegacyMeshLocalAddress(CallbackWithStatusArg1 cb)
{
	struct in6_addr legacy_addr;

	if ((mLegacyInterfaceEnabled || mNodeTypeSupportsLegacy)
	  && buffer_is_nonzero(mNCPV6LegacyPrefix, sizeof(mNCPV6LegacyPrefix))
	) {
		legacy_addr = make_slaac_addr_from_eui64(mNCPV6LegacyPrefix, mMACAddress);
		cb(kWPANTUNDStatus_Ok, boost::any(in6_addr_to_string(legacy_addr)));
	} else {
		cb(kWPANTUNDStatus_FeatureNotSupported, std::string("Property is unavailable"));
	}
}

void
NCPInstanceBase::get_prop_NCPState(CallbackWithStatusArg1 cb)
{
	if (is_initializing_ncp() && !ncp_state_is_detached_from_ncp(get_ncp_state())) {
		cb(kWPANTUNDStatus_Ok, boost::any(std::string(kWPANTUNDStateUninitialized)));
	} else {
		cb(kWPANTUNDStatus_Ok, boost::any(ncp_state_to_string(get_ncp_state())));
	}
}

void
NCPInstanceBase::get_prop_NetworkNodeType(CallbackWithStatusArg1 cb)
{
	cb(kWPANTUNDStatus_Ok, boost::any(node_type_to_string(mNodeType)));
}

void
NCPInstanceBase::get_prop_ThreadOnMeshPrefixes(CallbackWithStatusArg1 cb)
{
	std::list<std::string> result;
	std::multimap<IPv6Prefix, OnMeshPrefixEntry>::const_iterator iter;
	for (iter = mOnMeshPrefixes.begin(); iter != mOnMeshPrefixes.end(); iter++ ) {
		result.push_back(iter->second.get_description(iter->first, true));
	}
	cb(kWPANTUNDStatus_Ok, boost::any(result));
}

void
NCPInstanceBase::get_prop_ThreadOffMeshRoutes(CallbackWithStatusArg1 cb)
{
	std::list<std::string> result;
	std::multimap<IPv6Prefix, OffMeshRouteEntry>::const_iterator iter;
	for (iter = mOffMeshRoutes.begin(); iter != mOffMeshRoutes.end(); iter++ ) {
		result.push_back(iter->second.get_description(iter->first, true));
	}
	cb(kWPANTUNDStatus_Ok, boost::any(result));
}

void
NCPInstanceBase::get_prop_ThreadServices(CallbackWithStatusArg1 cb)
{
	std::list<std::string> result;
	std::vector<ServiceEntry>::const_iterator iter;

	for (iter = mServiceEntries.begin(); iter != mServiceEntries.end(); ++iter) {
		result.push_back(iter->get_description());
	}
	cb(kWPANTUNDStatus_Ok, boost::any(result));
}

void
NCPInstanceBase::get_prop_ThreadServicesAsValMap(CallbackWithStatusArg1 cb)
{
	std::list<ValueMap> result;
	std::vector<ServiceEntry>::const_iterator iter;
	ValueMap val_map;

	for (iter = mServiceEntries.begin(); iter != mServiceEntries.end(); ++iter) {
		val_map[kWPANTUNDValueMapKey_Service_EnterpriseNumber] = iter->get_enterprise_number();
		val_map[kWPANTUNDValueMapKey_Service_ServiceData] = iter->get_service_data();
		val_map[kWPANTUNDValueMapKey_Service_Stable] = iter->is_stable();
		val_map[kWPANTUNDValueMapKey_Service_ServerData] = iter->get_server_data();
		result.push_back(val_map);
	}
	cb(kWPANTUNDStatus_Ok, boost::any(result));
}

void
NCPInstanceBase::get_prop_IPv6AllAddresses(CallbackWithStatusArg1 cb)
{
	std::list<std::string> result;
	std::map<struct in6_addr, UnicastAddressEntry>::const_iterator iter;
	for (iter = mUnicastAddresses.begin(); iter != mUnicastAddresses.end(); iter++ ) {
		result.push_back(iter->second.get_description(iter->first, true));
	}
	cb(kWPANTUNDStatus_Ok, boost::any(result));
}

void
NCPInstanceBase::get_prop_IPv6MulticastAddresses(CallbackWithStatusArg1 cb)
{
	std::list<std::string> result;
	std::map<struct in6_addr, MulticastAddressEntry>::const_iterator iter;
	for (iter = mMulticastAddresses.begin(); iter != mMulticastAddresses.end(); iter++ ) {
		result.push_back(iter->second.get_description(iter->first, true));
	}
	cb(kWPANTUNDStatus_Ok, boost::any(result));
}

void
NCPInstanceBase::get_prop_IPv6InterfaceRoutes(CallbackWithStatusArg1 cb)
{
	std::list<std::string> result;
	std::map<IPv6Prefix, InterfaceRouteEntry>::const_iterator iter;
	for (iter = mInterfaceRoutes.begin(); iter != mInterfaceRoutes.end(); iter++ ) {
		result.push_back(iter->second.get_description(iter->first, true));
	}
	cb(kWPANTUNDStatus_Ok, boost::any(result));
}

void
NCPInstanceBase::get_prop_DaemonSyslogMask(CallbackWithStatusArg1 cb)
{
	std::string mask_string;
	int logmask;

	setlogmask(logmask = setlogmask(0));

	if (LOG_FAC(logmask) == LOG_DAEMON) {
		mask_string += "daemon ";
	}
	if (LOG_FAC(logmask) == LOG_USER) {
		mask_string += "user ";
	}
	if (logmask & LOG_MASK(LOG_EMERG)) {
		mask_string += "emerg ";
	}
	if (logmask & LOG_MASK(LOG_ALERT)) {
		mask_string += "alert ";
	}
	if (logmask & LOG_MASK(LOG_CRIT)) {
		mask_string += "crit ";
	}
	if (logmask & LOG_MASK(LOG_ERR)) {
		mask_string += "err ";
	}
	if (logmask & LOG_MASK(LOG_WARNING)) {
		mask_string += "warning ";
	}
	if (logmask & LOG_MASK(LOG_NOTICE)) {
		mask_string += "notice ";
	}
	if (logmask & LOG_MASK(LOG_INFO)) {
		mask_string += "info ";
	}
	if (logmask & LOG_MASK(LOG_DEBUG)) {
		mask_string += "debug ";
	}

	cb(kWPANTUNDStatus_Ok, mask_string);
}

// ----------------------------------------------------------------------------
// MARK: -
// MARK: Property Set Handlers

void
NCPInstanceBase::register_prop_set_handler(const char *prop, PropUpdateHandler handler)
{
	std::string key = to_upper(prop);
	mPropertySetHandlers[key] = PropUpdateHandlerEntry(prop, handler);
}

void
NCPInstanceBase::regsiter_all_set_handlers(void)
{
#define REGISTER_SET_HANDLER(name)     \
	register_prop_set_handler(kWPANTUNDProperty_##name, boost::bind(&NCPInstanceBase::set_prop_##name, this, _1, _2))

	REGISTER_SET_HANDLER(DaemonEnabled);
	REGISTER_SET_HANDLER(InterfaceUp);
	REGISTER_SET_HANDLER(StackUp);
	REGISTER_SET_HANDLER(MacFilterMode);
	REGISTER_SET_HANDLER(BCInterval);
	REGISTER_SET_HANDLER(UCDwellInterval);
	REGISTER_SET_HANDLER(BCDwellInterval);
	REGISTER_SET_HANDLER(UCChFunction);
	REGISTER_SET_HANDLER(BCChFunction);
	REGISTER_SET_HANDLER(UnicastChList);
	REGISTER_SET_HANDLER(BroadcastChList);
	REGISTER_SET_HANDLER(AsyncChList);
	REGISTER_SET_HANDLER(DaemonAutoAssociateAfterReset);
	REGISTER_SET_HANDLER(NestLabs_NetworkPassthruPort);
	REGISTER_SET_HANDLER(DaemonAutoFirmwareUpdate);
	REGISTER_SET_HANDLER(DaemonTerminateOnFault);
	REGISTER_SET_HANDLER(DaemonIPv6AutoUpdateIntfaceAddrOnNCP);
	REGISTER_SET_HANDLER(DaemonIPv6FilterUserAddedLinkLocal);
	REGISTER_SET_HANDLER(DaemonIPv6AutoAddSLAACAddress);
	REGISTER_SET_HANDLER(DaemonSetDefRouteForAutoAddedPrefix);
	REGISTER_SET_HANDLER(IPv6SetSLAACForAutoAddedPrefix);
	REGISTER_SET_HANDLER(DaemonOffMeshRouteAutoAddOnInterface);
	REGISTER_SET_HANDLER(DaemonOffMeshRouteFilterSelfAutoAdded);
	REGISTER_SET_HANDLER(DaemonOnMeshPrefixAutoAddAsIfaceRoute);
	REGISTER_SET_HANDLER(IPv6MeshLocalPrefix);
	REGISTER_SET_HANDLER(IPv6MeshLocalAddress);
	REGISTER_SET_HANDLER(DaemonAutoDeepSleep);
	REGISTER_SET_HANDLER(DaemonSyslogMask);

#undef REGISTER_SET_HANDLER
}

void
NCPInstanceBase::property_set_value(const std::string &key, const boost::any &value, CallbackWithStatus cb)
{
	// If we are disabled, then the only property we
	// are allowed to set is kWPANTUNDProperty_DaemonEnabled.
	if (!mEnabled && !strcaseequal(key.c_str(), kWPANTUNDProperty_DaemonEnabled)) {
		cb(kWPANTUNDStatus_InvalidWhenDisabled);
		return;
	}

	try {
		std::map<std::string, PropUpdateHandlerEntry>::iterator iter;

		iter = mPropertySetHandlers.find(to_upper(key));

		if (iter != mPropertySetHandlers.end()) {
			iter->second(value, cb);

		} else if (StatCollector::is_a_stat_property(key)) {
			get_stat_collector().property_set_value(key, value, cb);

		} else {
			syslog(LOG_ERR, "property_set_value: Unsupported property \"%s\"", key.c_str());
			cb(kWPANTUNDStatus_PropertyNotFound);
		}

	} catch (const boost::bad_any_cast &x) {
		// We will get a bad_any_cast exception if the property is of
		// the wrong type.
		syslog(LOG_ERR, "property_set_value: Bad type for property \"%s\" (%s)", key.c_str(), x.what());
		cb(kWPANTUNDStatus_InvalidArgument);

	} catch (const std::invalid_argument &x) {
		// We will get a bad_any_cast exception if the property is of
		// the wrong type.
		syslog(LOG_ERR, "property_set_value: Invalid argument for property \"%s\" (%s)", key.c_str(), x.what());
		cb(kWPANTUNDStatus_InvalidArgument);
	}
}

void
NCPInstanceBase::set_prop_DaemonEnabled(const boost::any &value, CallbackWithStatus cb)
{
	mEnabled = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_InterfaceUp(const boost::any &value, CallbackWithStatus cb)
{
	bool isup = any_to_bool(value);
	if (isup != mPrimaryInterface->is_online()) {
		if (isup) {
			get_control_interface().attach(cb);
		} else {
			if (ncp_state_is_joining_or_joined(get_ncp_state())) {
				// This isn't quite what we want, but the subclass
				// should be overriding this anyway.
				get_control_interface().reset();
			}
		}
	} else {
		cb(kWPANTUNDStatus_Ok);
	}
}

void NCPInstanceBase::set_prop_StackUp(const boost::any &value, CallbackWithStatus cb){
	bool isup = any_to_bool(value);
	if (isup != mStackUp) {
		if (isup) {
			get_control_interface().attach(cb);
		} else {
			if (ncp_state_is_joining_or_joined(get_ncp_state())) {
				get_control_interface().reset();
			}
		}
	} else {
		cb(kWPANTUNDStatus_Ok);
	}
}
void NCPInstanceBase::set_prop_MacFilterMode(const boost::any &value, CallbackWithStatus cb){
	mMacFilterMode = any_to_int(value);
	cb(kWPANTUNDStatus_Ok);
}
void NCPInstanceBase::set_prop_BCInterval(const boost::any &value, CallbackWithStatus cb){
	mBCInterval = any_to_int(value);
	cb(kWPANTUNDStatus_Ok);
}
void NCPInstanceBase::set_prop_UCDwellInterval(const boost::any &value, CallbackWithStatus cb){
	mUCDwellInterval = any_to_int(value);
	cb(kWPANTUNDStatus_Ok);
}
void NCPInstanceBase::set_prop_BCDwellInterval(const boost::any &value, CallbackWithStatus cb){
	mBCDwellInterval = any_to_int(value);
	cb(kWPANTUNDStatus_Ok);
}
void NCPInstanceBase::set_prop_UCChFunction(const boost::any &value, CallbackWithStatus cb){
	mUCChFunction = any_to_int(value);
	cb(kWPANTUNDStatus_Ok);
}
void NCPInstanceBase::set_prop_BCChFunction(const boost::any &value, CallbackWithStatus cb){
	mBCChFunction = any_to_int(value);
	cb(kWPANTUNDStatus_Ok);
}
void NCPInstanceBase::set_prop_UnicastChList(const boost::any &value, CallbackWithStatus cb){
	// reset bit mask
	for (int x = 0; x < 129; x++){
		mUnicastArray[x] = 0;
	}

	// convert input string to new bitmask
	convert_to_bitmask_unicast(any_to_string(value));

	std::string ret = "";
	for (int x = 0; x < 16; x++){
		char str_to_add[4];
		sprintf(str_to_add, "%02x:", mUnicastBytes[x]);
		ret.append(str_to_add);
	}
	char final_str_to_add[4];
	sprintf(final_str_to_add, "%02x", mUnicastBytes[16]);
	ret.append(final_str_to_add);
	mUnicastChList = ret;

	cb(kWPANTUNDStatus_Ok);
}
void NCPInstanceBase::set_prop_BroadcastChList(const boost::any &value, CallbackWithStatus cb){
	// reset bit mask
	for (int x = 0; x < 129; x++){
		mBroadcastArray[x] = 0;
	}

	// convert input string to new bitmask
	convert_to_bitmask_broadcast(any_to_string(value));

	std::string ret = "";
	for (int x = 0; x < 16; x++){
		char str_to_add[4];
		sprintf(str_to_add, "%02x:", mBroadcastBytes[x]);
		ret.append(str_to_add);
	}
	char final_str_to_add[4];
	sprintf(final_str_to_add, "%02x", mBroadcastBytes[16]);
	ret.append(final_str_to_add);
	mBroadcastChList = ret;

	cb(kWPANTUNDStatus_Ok);
}
void NCPInstanceBase::set_prop_AsyncChList(const boost::any &value, CallbackWithStatus cb){
	// reset bit mask
	for (int x = 0; x < 129; x++){
		mAsyncArray[x] = 0;
	}

	// convert input string to new bitmask
	convert_to_bitmask_async(any_to_string(value));

	std::string ret = "";
	for (int x = 0; x < 16; x++){
		char str_to_add[4];
		sprintf(str_to_add, "%02x:", mAsyncBytes[x]);
		ret.append(str_to_add);
	}
	char final_str_to_add[4];
	sprintf(final_str_to_add, "%02x", mAsyncBytes[16]);
	ret.append(final_str_to_add);
	mAsyncChList = ret;

	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonAutoAssociateAfterReset(const boost::any &value, CallbackWithStatus cb)
{
	mAutoResume = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_NestLabs_NetworkPassthruPort(const boost::any &value, CallbackWithStatus cb)
{
	mCommissionerPort = static_cast<uint16_t>(any_to_int(value));
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonAutoFirmwareUpdate(const boost::any &value, CallbackWithStatus cb)
{
	bool value_bool = any_to_bool(value);

	if (value_bool && !mAutoUpdateFirmware) {
		if (get_ncp_state() == FAULT) {
			syslog(LOG_ALERT, "The NCP is misbehaving: Attempting a firmware update");
			upgrade_firmware();
		} else if (get_ncp_state() != UNINITIALIZED) {
			if (is_firmware_upgrade_required(mNCPVersionString)) {
				syslog(LOG_NOTICE, "NCP FIRMWARE UPGRADE IS REQUIRED");
				upgrade_firmware();
			}
		}
	}

	mAutoUpdateFirmware = value_bool;
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonTerminateOnFault(const boost::any &value, CallbackWithStatus cb)
{
	mTerminateOnFault = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
	if (mTerminateOnFault && (get_ncp_state() == FAULT)) {
		reinitialize_ncp();
	}
}

void
NCPInstanceBase::set_prop_DaemonIPv6AutoUpdateIntfaceAddrOnNCP(const boost::any &value, CallbackWithStatus cb)
{
	mAutoUpdateInterfaceIPv6AddrsOnNCP = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonIPv6FilterUserAddedLinkLocal(const boost::any &value, CallbackWithStatus cb)
{
	mFilterUserAddedLinkLocalIPv6Address = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonIPv6AutoAddSLAACAddress(const boost::any &value, CallbackWithStatus cb)
{
	mAutoAddSLAACAddress = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonSetDefRouteForAutoAddedPrefix(const boost::any &value, CallbackWithStatus cb)
{
	mSetDefaultRouteForAutoAddedPrefix = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_IPv6SetSLAACForAutoAddedPrefix(const boost::any &value, CallbackWithStatus cb)
{
	mSetSLAACForAutoAddedPrefix = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonOffMeshRouteAutoAddOnInterface(const boost::any &value, CallbackWithStatus cb)
{
	mAutoAddOffMeshRoutesOnInterface = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonOffMeshRouteFilterSelfAutoAdded(const boost::any &value, CallbackWithStatus cb)
{
	mFilterSelfAutoAddedOffMeshRoutes = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_DaemonOnMeshPrefixAutoAddAsIfaceRoute(const boost::any &value, CallbackWithStatus cb)
{
	mAutoAddOnMeshPrefixesAsInterfaceRoutes = any_to_bool(value);
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::set_prop_IPv6MeshLocalPrefix(const boost::any &value, CallbackWithStatus cb)
{
	if (get_ncp_state() <= OFFLINE) {
		nl::Data prefix;

		if (value.type() == typeid(std::string)) {
			uint8_t ula_bytes[16] = {};
			const std::string ip_string(any_to_string(value));

			// Address-style
			int bits = inet_pton(AF_INET6,ip_string.c_str(),ula_bytes);
			if (bits <= 0) {
				// Prefix is the wrong length.
				cb(kWPANTUNDStatus_InvalidArgument);
				return;
			}

			prefix = nl::Data(ula_bytes, 8);
		} else {
			prefix = any_to_data(value);
		}

		if (prefix.size() < sizeof(mNCPV6Prefix)) {
			// Prefix is the wrong length.
			cb(kWPANTUNDStatus_InvalidArgument);
		}
		memcpy(mNCPV6Prefix, prefix.data(), sizeof(mNCPV6Prefix));
		cb(kWPANTUNDStatus_Ok);
	} else {
		cb(kWPANTUNDStatus_InvalidForCurrentState);
	}
}

void
NCPInstanceBase::set_prop_IPv6MeshLocalAddress(const boost::any &value, CallbackWithStatus cb)
{
	set_prop_IPv6MeshLocalPrefix(value, cb);
}

void
NCPInstanceBase::set_prop_DaemonAutoDeepSleep(const boost::any &value, CallbackWithStatus cb)
{
	mAutoDeepSleep = any_to_bool(value);

	if (mAutoDeepSleep == false
		&& mNCPState == DEEP_SLEEP
		&& mEnabled
	) {
		// Wake us up if we are asleep and deep sleep was turned off.
		get_control_interface().refresh_state(boost::bind(cb, kWPANTUNDStatus_Ok));
	} else {
		cb(kWPANTUNDStatus_Ok);
	}
}

void
NCPInstanceBase::set_prop_DaemonSyslogMask(const boost::any &value, CallbackWithStatus cb)
{
#if !FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
	setlogmask(strtologmask(any_to_string(value).c_str(), setlogmask(0)));
#endif
	cb(kWPANTUNDStatus_Ok);

}

// ----------------------------------------------------------------------------
// MARK: -
// MARK: Property Insert Handlers

void
NCPInstanceBase::register_prop_insert_handler(const char *prop, PropUpdateHandler handler)
{
	std::string key = to_upper(prop);
	mPropertyInsertHandlers[key] = PropUpdateHandlerEntry(prop, handler);
}

void
NCPInstanceBase::regsiter_all_insert_handlers(void)
{
#define REGISTER_INSERT_HANDLER(name)                                         \
	register_prop_insert_handler(                                             \
		kWPANTUNDProperty_##name,                                             \
		boost::bind(&NCPInstanceBase::insert_prop_##name, this, _1, _2))

	REGISTER_INSERT_HANDLER(IPv6MulticastAddresses);
	REGISTER_INSERT_HANDLER(MacFilterList);

#undef REGISTER_INSERT_HANDLER
}

void
NCPInstanceBase::property_insert_value(const std::string &key, const boost::any &value, CallbackWithStatus cb)
{
	try {
		std::map<std::string, PropUpdateHandlerEntry>::iterator iter;

		iter = mPropertyInsertHandlers.find(to_upper(key));

		if (iter != mPropertyInsertHandlers.end()) {
			iter->second(value, cb);

		} else {
			syslog(LOG_ERR, "property_insert_value: Property not supported or not insert-value capable \"%s\"", key.c_str());
			cb(kWPANTUNDStatus_PropertyNotFound);
		}

	} catch (const boost::bad_any_cast &x) {
		// We will get a bad_any_cast exception if the property is of
		// the wrong type.
		syslog(LOG_ERR,"property_insert_value: Bad type for property \"%s\" (%s)", key.c_str(), x.what());
		cb(kWPANTUNDStatus_InvalidArgument);
	} catch (const std::invalid_argument &x) {
		// We will get a bad_any_cast exception if the property is of
		// the wrong type.
		syslog(LOG_ERR,"property_insert_value: Invalid argument for property \"%s\" (%s)", key.c_str(), x.what());
		cb(kWPANTUNDStatus_InvalidArgument);
	}
}

void
NCPInstanceBase::insert_prop_IPv6MulticastAddresses(const boost::any &value, CallbackWithStatus cb)
{
	struct in6_addr address = any_to_ipv6(value);
	multicast_address_was_joined(kOriginUser, address, cb);
}

void NCPInstanceBase::insert_prop_MacFilterList(const boost::any &value, CallbackWithStatus cb){
	convert_to_filter_list(any_to_string(value));
	cb(kWPANTUNDStatus_Ok);
}

// ----------------------------------------------------------------------------
// MARK: -
// MARK: Property Remove Handlers

void
NCPInstanceBase::register_prop_remove_handler(const char *prop, PropUpdateHandler handler)
{
	std::string key = to_upper(prop);
	mPropertyRemoveHandlers[key] = PropUpdateHandlerEntry(prop, handler);
}

void
NCPInstanceBase::regsiter_all_remove_handlers(void)
{
#define REGISTER_REMOVE_HANDLER(name)                                         \
	register_prop_remove_handler(                                             \
		kWPANTUNDProperty_##name,                                             \
		boost::bind(&NCPInstanceBase::remove_prop_##name, this, _1, _2))

	REGISTER_REMOVE_HANDLER(IPv6MulticastAddresses);
	REGISTER_REMOVE_HANDLER(MacFilterList);

#undef REGISTER_REMOVE_HANDLER
}

void
NCPInstanceBase::property_remove_value(const std::string &key, const boost::any &value, CallbackWithStatus cb)
{
	try {
		std::map<std::string, PropUpdateHandlerEntry>::iterator iter;

		iter = mPropertyRemoveHandlers.find(to_upper(key));

		if (iter != mPropertyRemoveHandlers.end()) {
			iter->second(value, cb);

		} else {
			syslog(LOG_ERR, "property_remove_value: Property not supported or not remove-value capable \"%s\"", key.c_str());
			cb(kWPANTUNDStatus_PropertyNotFound);
		}
	} catch (const boost::bad_any_cast &x) {
		// We will get a bad_any_cast exception if the property is of
		// the wrong type.
		syslog(LOG_ERR,"property_remove_value: Bad type for property \"%s\" (%s)", key.c_str(), x.what());
		cb(kWPANTUNDStatus_InvalidArgument);
	} catch (const std::invalid_argument &x) {
		// We will get a bad_any_cast exception if the property is of
		// the wrong type.
		syslog(LOG_ERR,"property_remove_value: Invalid argument for property \"%s\" (%s)", key.c_str(), x.what());
		cb(kWPANTUNDStatus_InvalidArgument);
	}
}

void
NCPInstanceBase::remove_prop_IPv6MulticastAddresses(const boost::any &value, CallbackWithStatus cb)
{
	struct in6_addr address = any_to_ipv6(value);
	multicast_address_was_left(kOriginUser, address, cb);
}

void NCPInstanceBase::remove_prop_MacFilterList(const boost::any &value, CallbackWithStatus cb){
	convert_to_filter_list(any_to_string(value));
	cb(kWPANTUNDStatus_Ok);
}

void
NCPInstanceBase::signal_property_changed(
	const std::string& key,
	const boost::any& value
) {
	get_control_interface().mOnPropertyChanged(key, value);
}

// ----------------------------------------------------------------------------
// MARK: -

void
NCPInstanceBase::set_initializing_ncp(bool x)
{
	if (mIsInitializingNCP != x) {
		mIsInitializingNCP = x;

		if (mIsInitializingNCP) {
			change_ncp_state(UNINITIALIZED);
			set_ncp_power(true);
		} else if ( (get_ncp_state() != UNINITIALIZED)
				 && (get_ncp_state() != FAULT)
				 && (get_ncp_state() != UPGRADING)
		) {
			handle_ncp_state_change(get_ncp_state(), UNINITIALIZED);
		}
	}
}

bool
NCPInstanceBase::is_initializing_ncp(void) const
{
	return mIsInitializingNCP;
}

NCPState
NCPInstanceBase::get_ncp_state()const
{
	return mNCPState;
}

bool
NCPInstanceBase::is_state_change_valid(NCPState new_ncp_state) const
{
	// Add any invalid state transitions here so that
	// bugs can be more quickly identified and corrected.

	if (ncp_state_is_detached_from_ncp(get_ncp_state())) {
		return new_ncp_state == UNINITIALIZED;
	}

	return true;
}

void
NCPInstanceBase::change_ncp_state(NCPState new_ncp_state)
{
	NCPState old_ncp_state = mNCPState;

	if (old_ncp_state != new_ncp_state) {
		if (!is_state_change_valid(new_ncp_state)) {
			syslog(
				LOG_WARNING,
				"BUG: Invalid state change: \"%s\" -> \"%s\"",
				ncp_state_to_string(old_ncp_state).c_str(),
				ncp_state_to_string(new_ncp_state).c_str()
			);

			if (ncp_state_is_detached_from_ncp(get_ncp_state())) {
				// If the state was detached, do not allow the
				// state change to continue.
				return;
			}
		} else {
			syslog(
				LOG_NOTICE,
				"State change: \"%s\" -> \"%s\"",
				ncp_state_to_string(old_ncp_state).c_str(),
				ncp_state_to_string(new_ncp_state).c_str()
			);
		}

		mNCPState = new_ncp_state;

		if ( !mIsInitializingNCP
		  || (new_ncp_state == UNINITIALIZED)
		  || (new_ncp_state == FAULT)
		  || (new_ncp_state == UPGRADING)
		) {
			handle_ncp_state_change(new_ncp_state, old_ncp_state);
		}
	}
}

void
NCPInstanceBase::handle_ncp_state_change(NCPState new_ncp_state, NCPState old_ncp_state)
{
	// Detached NCP -> Online NCP
	if (ncp_state_is_detached_from_ncp(old_ncp_state)
	 && !ncp_state_is_detached_from_ncp(new_ncp_state)
	) {
		__ASSERT_MACROS_check(new_ncp_state == UNINITIALIZED);

		// We are transitioning out of a state where we are disconnected
		// from the NCP. This requires a hard reset.
		set_ncp_power(true);

		if (mResetSocket != NULL) {
			// If we have a way to hard reset the NCP,
			// then do it. We do the check above to make
			// sure that we don't end up calling mSerialAdapter->reset()
			// twice.
			hard_reset_ncp();
		}

		mSerialAdapter->reset();

		PT_INIT(&mControlPT);
	}

	// Online NCP -> Detached NCP
	else if (!ncp_state_is_detached_from_ncp(old_ncp_state)
	 && ncp_state_is_detached_from_ncp(new_ncp_state)
	) {
		// We are transitioning into a state where we need to be disconnected
		// from the NCP. For this we use the hibernate command.
		mSerialAdapter->hibernate();
		PT_INIT(&mControlPT);
		NLPT_INIT(&mDriverToNCPPumpPT);
		NLPT_INIT(&mNCPToDriverPumpPT);
		mFailureCount = 0;

		if (new_ncp_state == FAULT) {
			// When we enter the fault state, attempt to
			// ensure that we are using as little power as
			// possible by physically turning off the NCP
			// (if a method of doing so has been specified
			// in our configuration)
			set_ncp_power(false);

			if (mTerminateOnFault) {
				signal_fatal_error(kWPANTUNDStatus_Failure);
			}
		}
		return;
	}

	// Interface Down -> Interface Up
	if (!ncp_state_is_interface_up(old_ncp_state)
	 && ncp_state_is_interface_up(new_ncp_state)
	) {
		set_online(true);


	// InterfaceUp -> COMMISSIONED
	// (Special case of InterfaceUp -> InterfaceDown)
	} else if (ncp_state_is_interface_up(old_ncp_state)
				&& (new_ncp_state == COMMISSIONED)
				&& mAutoResume
				// The `mEnabled` check covers the case where driver is disabled
				// causing a transition to COMMISSIONED state. In that case
				// we want to fall to "InterfaceUp -> InterfaceDown (General Case)"
				// to take the interface down.
				&& mEnabled
	) {
		// We don't bother going further if autoresume is on.
		return;


	// Commissioned -> InterfaceDown
	// (Special case of InterfaceUp -> InterfaceDown)
	} else if (ncp_state_is_commissioned(old_ncp_state)
		&& !ncp_state_is_commissioned(new_ncp_state)
		&& !ncp_state_is_joining(new_ncp_state)
		&& !ncp_state_is_sleeping(new_ncp_state)
		&& (new_ncp_state != UNINITIALIZED)
	) {
		reset_interface();


	// Uninitialized -> Offline
	// If we are transitioning from uninitialized to offline,
	// we clear all addresses/prefixes.
	} else if (old_ncp_state == UNINITIALIZED
		&& new_ncp_state == OFFLINE
	) {
		reset_interface();


	// InterfaceUp -> InterfaceDown (General Case)
	} else if (ncp_state_is_interface_up(old_ncp_state)
		&& !ncp_state_is_interface_up(new_ncp_state)
		&& new_ncp_state != NET_WAKE_WAKING
	) {
		// Take the interface offline.
		syslog(LOG_NOTICE, "Taking interface(s) down. . .");

		mCurrentNetworkInstance.joinable = false;
		set_commissioniner(0, 0, 0);
		set_online(false);
	}

	// We don't announce transitions to the "UNITIALIZED" state.
	if (UNINITIALIZED != new_ncp_state) {
		signal_property_changed(kWPANTUNDProperty_NCPState, ncp_state_to_string(new_ncp_state));
	}

	mNetworkRetain.handle_ncp_state_change(new_ncp_state, old_ncp_state);
}

void
NCPInstanceBase::reinitialize_ncp(void)
{
	PT_INIT(&mControlPT);
	change_ncp_state(UNINITIALIZED);
}

void
NCPInstanceBase::reset_tasks(wpantund_status_t status)
{
}

// ----------------------------------------------------------------------------
// MARK: Network Time Update

void
NCPInstanceBase::handle_network_time_update(const ValueMap &update)
{
	get_control_interface().mOnNetworkTimeUpdate(update);
}

// ----------------------------------------------------------------------------
// MARK: -

bool
NCPInstanceBase::is_busy(void)
{
	const NCPState ncp_state = get_ncp_state();
	bool is_busy = ncp_state_is_busy(ncp_state);

	if (is_initializing_ncp()) {
		return true;
	}

	if (ncp_state == FAULT) {
		return false;
	}

	if (get_upgrade_status() == EINPROGRESS) {
		is_busy = true;
	}

	return is_busy;
}

StatCollector&
NCPInstanceBase::get_stat_collector(void)
{
	return mStatCollector;
}

void
NCPInstanceBase::update_busy_indication(void)
{
	cms_t current_time = time_ms();

	if (mWasBusy != is_busy()) {
		if (!mWasBusy
			|| (mLastChangedBusy == 0)
			|| (current_time - mLastChangedBusy >= BUSY_DEBOUNCE_TIME_IN_MS)
			|| (current_time - mLastChangedBusy < 0)
		) {
			mWasBusy = !mWasBusy;
			if(!mWasBusy) {
				if (mLastChangedBusy == 0) {
					syslog(LOG_INFO, "NCP is no longer busy, host sleep is permitted.");
				} else {
					syslog(LOG_INFO, "NCP is no longer busy, host sleep is permitted. (Was busy for %dms)",(int)(current_time - mLastChangedBusy));
				}
				signal_property_changed(kWPANTUNDProperty_DaemonReadyForHostSleep, true);
			} else {
				syslog(LOG_INFO, "NCP is now BUSY.");
				signal_property_changed(kWPANTUNDProperty_DaemonReadyForHostSleep, false);
			}
			mLastChangedBusy = current_time;
		}
	} else if (mWasBusy
		&& (mLastChangedBusy != 0)
		&& (current_time - mLastChangedBusy > MAX_INSOMNIA_TIME_IN_MS)
	) {
		//syslog(LOG_ERR, "Experiencing extended insomnia. Resetting internal state.");

		mLastChangedBusy = current_time;

		//ncp_is_misbehaving();
	}
}

void
NCPInstanceBase::ncp_is_misbehaving(void)
{
	mNCPIsMisbehaving = true;
}

// ----------------------------------------------------------------------------
// MARK: -

bool
NCPInstanceBase::is_firmware_upgrade_required(const std::string& version)
{
	return mFirmwareUpgrade.is_firmware_upgrade_required(version);
}

void
NCPInstanceBase::upgrade_firmware(void)
{
	change_ncp_state(UPGRADING);

	set_ncp_power(true);

	return mFirmwareUpgrade.upgrade_firmware();
}

int
NCPInstanceBase::get_upgrade_status(void)
{
	return mFirmwareUpgrade.get_upgrade_status();
}

bool
NCPInstanceBase::can_upgrade_firmware(void)
{
	return mFirmwareUpgrade.can_upgrade_firmware();
}

void 
NCPInstanceBase::convert_to_bitmask_unicast(std::string value){
	// step 1 : split input string
	std::vector<std::string> values;
	int start = 0;
	int end = value.find(':');
	while(end != -1){
		values.push_back(value.substr(start, end - start));
		start = end + 1;
		end = value.find(':', start);
	}
	values.push_back(value.substr(start, end - start));

	// step 2 : get channel values
	for (int x = 0; x < values.size(); x++){
		int start_channel = 0;
		int end_channel = 0;
		end = values.at(x).find('-');
		if (end == -1){
			// just a single channel
			start_channel = any_to_int(values.at(x));
			end_channel = start_channel;
		}
		else{
			// multiple channels
			start_channel = any_to_int(values.at(x).substr(0, end));
			end_channel = any_to_int(values.at(x).substr(end + 1, (values.at(x).length() - 1 - end)));
		}

		for (int y = 0; y <= (end_channel - start_channel); y++){
			mUnicastArray[start_channel + y] = 1;
		}
	}

	// step 3 : convert list of channel value bits to 17 bytes
	int eight_multiple = 8;
	int channel_mask_byte = 0;
	int count = 0;
	int value_to_store = 0;
	for (int x = 0; x < 129; x++){
		if (x < eight_multiple){
			value_to_store += (mUnicastArray[x] << count);
			count++;
		}
		else{
			mUnicastBytes[eight_multiple/8 - 1] = value_to_store;
			eight_multiple += 8;
			count = 0;
			value_to_store = 0;

			// need to account for first bit in each byte
			value_to_store += (mUnicastArray[x] << count);
			count++;
		}
	}
}
void 
NCPInstanceBase::convert_to_bitmask_broadcast(std::string value){
	// step 1 : split input string
	std::vector<std::string> values;
	int start = 0;
	int end = value.find(':');
	while(end != -1){
		values.push_back(value.substr(start, end - start));
		start = end + 1;
		end = value.find(':', start);
	}
	values.push_back(value.substr(start, end - start));

	// step 2 : get channel values
	for (int x = 0; x < values.size(); x++){
		int start_channel = 0;
		int end_channel = 0;
		end = values.at(x).find('-');
		if (end == -1){
			// just a single channel
			start_channel = any_to_int(values.at(x));
			end_channel = start_channel;
		}
		else{
			// multiple channels
			start_channel = any_to_int(values.at(x).substr(0, end));
			end_channel = any_to_int(values.at(x).substr(end + 1, (values.at(x).length() - 1 - end)));
		}

		for (int y = 0; y <= (end_channel - start_channel); y++){
			mBroadcastArray[start_channel + y] = 1;
		}
	}

	// step 3 : convert list of channel value bits to 17 bytes
	int eight_multiple = 8;
	int channel_mask_byte = 0;
	int count = 0;
	int value_to_store = 0;
	for (int x = 0; x < 129; x++){
		if (x < eight_multiple){
			value_to_store += (mBroadcastArray[x] << count);
			count++;
		}
		else{
			mBroadcastBytes[eight_multiple/8 - 1] = value_to_store;
			eight_multiple += 8;
			count = 0;
			value_to_store = 0;

			// need to account for first bit in each byte
			value_to_store += (mBroadcastArray[x] << count);
			count++;
		}
	}
}
void 
NCPInstanceBase::convert_to_bitmask_async(std::string value){
	// step 1 : split input string
	std::vector<std::string> values;
	int start = 0;
	int end = value.find(':');
	while(end != -1){
		values.push_back(value.substr(start, end - start));
		start = end + 1;
		end = value.find(':', start);
	}
	values.push_back(value.substr(start, end - start));

	// step 2 : get channel values
	for (int x = 0; x < values.size(); x++){
		int start_channel = 0;
		int end_channel = 0;
		end = values.at(x).find('-');
		if (end == -1){
			// just a single channel
			start_channel = any_to_int(values.at(x));
			end_channel = start_channel;
		}
		else{
			// multiple channels
			start_channel = any_to_int(values.at(x).substr(0, end));
			end_channel = any_to_int(values.at(x).substr(end + 1, (values.at(x).length() - 1 - end)));
		}

		for (int y = 0; y <= (end_channel - start_channel); y++){
			mAsyncArray[start_channel + y] = 1;
		}
	}

	// step 3 : convert list of channel value bits to 17 bytes
	int eight_multiple = 8;
	int channel_mask_byte = 0;
	int count = 0;
	int value_to_store = 0;
	for (int x = 0; x < 129; x++){
		if (x < eight_multiple){
			value_to_store += (mAsyncArray[x] << count);
			count++;
		}
		else{
			mAsyncBytes[eight_multiple/8 - 1] = value_to_store;
			eight_multiple += 8;
			count = 0;
			value_to_store = 0;

			// need to account for first bit in each byte
			value_to_store += (mAsyncArray[x] << count);
			count++;
		}
	}
}

/* Custom revstr for macfilterlist */
void custom_revstr(char *str1)  
{  
    // declare variable  
    int i, len, temp1, temp2;  
    len = strlen(str1); // use strlen() to get the length of str string  
      
    // use for loop to iterate the string   
    for (i = 0; i < len/2; i+=2)  
    {  
        // temp variable use to temporary hold the string  
        temp1 = str1[i];  
		temp2 = str1[i+1]; 
        str1[i] = str1[len - i - 2];  
		str1[i+1] = str1[len - i - 1];  
        str1[len - i - 1] = temp2;  
		str1[len - i - 2] = temp1;
    }  
}  

void 
NCPInstanceBase::convert_to_filter_list(std::string value){
	// Store MAC address (16 digit hex) into two ints (32bit each)
	int first_input_int;
	int second_input_int;
	if (value.substr(0,3) == "add" || value.substr(0,3) == "rem"){
			std::string string_to_insert = value.substr(3, 8);
			char * p1;
			first_input_int = strtol(string_to_insert.c_str(), & p1, 16);

			string_to_insert = value.substr(11, 8);
			char * p2;
			second_input_int = strtol(string_to_insert.c_str(), & p2, 16);
	}

	if (value.substr(0,3) == "add"){
		// perform add
		// step 1 : check if table is full
		int num_empty = 0;
		int first_empty = 10;
		for (int x = 0; x < MAC_FILTER_LIST_SIZE; x++){
			if (mMacFilterListString[x] == "0000000000000000"){
				num_empty++;
				if (x < first_empty){
					// get first empty row
					first_empty = x;
				}
			}
		}
		if (num_empty != 0){
			// row left to fill
			mMacFilterList[first_empty * 2] = first_input_int;
			mMacFilterList[first_empty * 2 + 1] = second_input_int;
		}
	}
	else if (value.substr(0,3) == "rem"){
		// perform remove
		// step 1 : find row to remove
		int row_remove = 10;
		for (int x = 0; x < MAC_FILTER_LIST_SIZE; x++){
			if ((mMacFilterList[x*2] == first_input_int) && (mMacFilterList[x*2+1] == second_input_int)){
				row_remove = x;
			}
		}
		if (row_remove != 10){
			// a row to remove was found
			mMacFilterList[row_remove * 2] = 0;
			mMacFilterList[row_remove * 2 + 1] = 0;
		}
	}

	// update the string array with new filter list
	std::string ret_string = "";
	int string_count = 0;

	for (int x = 0; x < (MAC_FILTER_LIST_SIZE * 2); x += 2){
		char str_to_add[17];
		sprintf(str_to_add, "%08x", mMacFilterList[x]);
		custom_revstr(str_to_add);
		ret_string = str_to_add;
		sprintf(str_to_add, "%08x", mMacFilterList[x + 1]);
		custom_revstr(str_to_add);
		ret_string.append(str_to_add);
		mMacFilterListString[string_count] = ret_string;
		string_count ++;
	}

}
