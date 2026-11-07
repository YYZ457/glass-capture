#include "VkCamera.h"

// Cn:枚举设备 | En:Enumerate Devices
DeviceMap EnumerateDevices(unsigned int type) {
	DeviceMap devMap;

	int devNum = VkEnumCameraByType(type);
	if (devNum <= 0) return devMap;

	for (int i = 0; i < devNum; i++)
	{
		char devName[256] = { 0 };
		VkGetCameraNameByIndex(i, devName, 256);

		CAMERA devHandle = VkGetCameraByName(devName);
		if (!devHandle) continue;
		unsigned int devType = VkGetCameraType(devHandle);

		devMap.emplace(devHandle, DeviceInfo{ devName, devType });
	}
	return devMap;
}


VkCamera::VkCamera(CAMERA handle) {
	m_cameraHandle = handle;
}

VkCamera::~VkCamera() {
	if (m_cameraHandle) {
		if (VkIsConnected(m_cameraHandle) == OK)
		{
			VkDisconnectCamera(m_cameraHandle);
		}
		VkCameraRelease(m_cameraHandle);
		m_cameraHandle = nullptr;

		VkDeviceRelease();
	}
}

// Cn:连接设备 | En:Connect device
int VkCamera::Connect(DisConnectCallbackFunc callback, void *context) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;
	if (callback == nullptr) return ERROR_INVALID_PARA;

	if (VkIsConnected(m_cameraHandle) == OK)
	{
		return ERROR_ALREDY_CONNECTED;
	}
	return VkConnectCamera(m_cameraHandle, callback, context);
}

// Cn:断开设备 | En:Disconnect device
int VkCamera::Disconnect() {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	if (VkIsConnected(m_cameraHandle) != OK)
	{
		return ERROR_ALREADY_DISCONNECTED;
	}
	return VkDisconnectCamera(m_cameraHandle);
}

// Cn:判断相机是否处于连接状态 | En:Is camera connected
int VkCamera::IsDeviceConnected() {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkIsConnected(m_cameraHandle);
}

// Cn:释放相机句柄 | En:Release camera handle
int VkCamera::ReleaseCameraHandle() {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	if (VkIsConnected(m_cameraHandle) == OK)
	{
		VkDisconnectCamera(m_cameraHandle);
	}
	VkCameraRelease(m_cameraHandle);
	m_cameraHandle = nullptr;
	return OK;
}

// Cn:释放设备资源 | En:Release device resources
int VkCamera::ReleaseDeviceResources() {
	return VkDeviceRelease();
}

// Cn:注册抓图回调 | En:Register image callback
// @note Cn:GRAB_MODE_MANUAL模式也需在开始取流之前注册图像回调（但实际从VkGetOneFrame取图）
//       En:GRAB_MODE_MANUAL mode also requires registering an image callback before starting acquisition (but actually retrieves images from VkGetOneFrame)
int VkCamera::RegisterImageCallback(unsigned int channel, CallbackFunc func) {
	return VkRegisterCallBack(m_cameraHandle, channel, func);
}

// Cn:设置采集模式 | En:Set grab mode
// @note Cn:GRAB_MODE_CALLBACK（自动通过回调函数传输图像）/GRAB_MODE_MANUAL（需主动调用VkGetOneFrame获取图像）
//	     En:GRAB_MODE_CALLBACK(Images auto-delivered via callback)/GRAB_MODE_MANUAL(Actively call VkGetOneFrame to retrieve images)
int VkCamera::SetAcquisitionMode(unsigned int grabMode) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkSetGrabMode(m_cameraHandle, grabMode);
}

// Cn:配置可变帧长功能 | En:Configure camera variable frame length function
// @note Cn:使用线阵相机时需启用可变帧长功能
//		 En:When using a line scan camera, the variable frame length function must be enabled.
int VkCamera::SupportVariableFrame(unsigned int isSupport) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkSupportVariableFrame(m_cameraHandle, isSupport);
}

// Cn:开启抓图 | En:Start Grabbing
int VkCamera::StartImageAcquisition(unsigned int timeout, unsigned int channel, void *context) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkStartGrab(m_cameraHandle, timeout, channel, context);
}

// Cn:停止抓图 | En:Stop Grabbing
int VkCamera::StopImageAcquisition(unsigned int channel) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkStopGrab(m_cameraHandle, channel);
}

// Cn:在手动模式下获取图像 | En:Acquire images in manual mode
int VkCamera::AcquireSingleFrame(unsigned int channel, unsigned int timeout, IMAGE* pImage) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetOneFrame(m_cameraHandle, channel, timeout, pImage);
}

// Cn:获取图像信息 | En:Get image information
int VkCamera::GetImageInfo(IMAGE image, PIMAGEINFO imageInfo) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetImageInfo(image, imageInfo);
}

// Cn:获取相机帧统计信息 | En:Get camera frame statistics
int VkCamera::GetFrameStatistics(STATISTICS statisticsInfo) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetCameraStatistics(m_cameraHandle, statisticsInfo);
}

// Cn:获取图像数据 | En:Get image data
unsigned char *VkCamera::GetImageBuffer(IMAGE Image) {
	return VkGetImageBuff(Image);
}

// Cn:获取图像数据长度 | En:Get image data length
int VkCamera::GetBufferNeed(IMAGE image) {
	return VkGetBuffNeed(image);
}

// Cn:释放图像缓存 | En:Release image buffer
void VkCamera::ReleaseImageBuffer(IMAGE image) {
	VkImageRelease(image);
}

// Cn:将图像格式转换为8位图像 | En:Convert the image format to 8-bit image
IMAGE VkCamera::Unpack2Image8(IMAGE image) {
	return VkUnpack2Image8(image);
}

// Cn:判断相机是否正在输出图像 | En:Is camera grabbing
int VkCamera::IsAcquisitionActive() {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkIsGrabbing(m_cameraHandle);
}

// Cn:保存相机图像到文件 | En:Save image to file
int VkCamera::SaveImage(IMAGE_META* pImage, SAVE_IMAGE_PARAM* pSaveParam, const char* pImagePath) {
	if (pImage == nullptr || pSaveParam == nullptr || pImagePath == nullptr) return ERROR_INVALID_PARA;

	return VkWriteImage(pImage, pSaveParam, pImagePath);
}

// Cn:保存相机配置文件到本地 | En:Save camera configuration file to local
int VkCamera::ConfigSave(const char *cfgFileName) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkConfigSave(m_cameraHandle, cfgFileName);
}

// Cn:加载相机配置文件到设备 | En:Load camera configuration file to device
int VkCamera::ConfigLoad(const char *cfgFileName) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkConfigLoad(m_cameraHandle, cfgFileName);
}

// Cn:获取Int型参数 | En:Get Int type parameters
int VkCamera::GetIntValue(const char *featureName, unsigned long long *featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetFeatureInt(m_cameraHandle, featureName, featureValue);
}

// Cn:设置Int型参数 | En:Set Int type parameters
int VkCamera::SetIntValue(const char *featureName, unsigned long long featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkSetFeatureInt(m_cameraHandle, featureName, featureValue);
}



// Cn:获取Enum型参数 | En:Get Enum type parameters
int VkCamera::GetEnumValue(const char *featureName, char *featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetFeatureEnum(m_cameraHandle, featureName, featureValue);
}

// Cn:设置Enum型参数 | En:Set Enum type parameters
int VkCamera::SetEnumValue(const char *featureName, const char* featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkSetFeatureEnum(m_cameraHandle, featureName, featureValue);
}

int VkCamera::GetFeatureEnumArray(const char *featureName, PENUMARRAY enumarray, int *enumCount) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetFeatureEnumArray(m_cameraHandle, featureName, enumarray, enumCount);
}

// Cn:获取Double型参数 | En:Get Double type parameters
int VkCamera::GetDoubleValue(const char *featureName, double *featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetFeatureDouble(m_cameraHandle, featureName, featureValue);
}

// Cn:设置Double型参数 | En:Set Double type parameters
int VkCamera::SetDoubleValue(const char *featureName, double featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkSetFeatureDouble(m_cameraHandle, featureName, featureValue);
}

// Cn:获取Bool型参数 | En:Get Bool type parameter
int VkCamera::GetBoolValue(const char *featureName, unsigned int *featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetFeatureBoolean(m_cameraHandle, featureName, featureValue);
}

// Cn:设置Bool型参数 | En:Set Bool type parameter
int VkCamera::SetBoolValue(const char *featureName, unsigned int featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkSetFeatureBoolean(m_cameraHandle, featureName, featureValue);
}

// Cn:获取String型参数 | En:Get String type parameter
int VkCamera::GetStringValue(const char *featureName, unsigned char *featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkGetFeatureString(m_cameraHandle, featureName, featureValue);
}

// Cn:设置String型参数 | En:Set String type parameter
int VkCamera::SetStringValue(const char *featureName, const char *featureValue) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkSetFeatureString(m_cameraHandle, featureName, featureValue);
}

// Cn:执行一次Command型命令 | En:Execute Command once
int VkCamera::ExecuteCommand(const char *featureName) {
	if (m_cameraHandle == nullptr) return ERROR_INVALID_ORDER;

	return VkExecuteFeatureCommand(m_cameraHandle, featureName);
}