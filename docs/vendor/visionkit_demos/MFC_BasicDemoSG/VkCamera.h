#ifndef _VK_CAMERA_H_
#define _VK_CAMERA_H_

#include "VkApiDefination.h"
#include "VkImageIO.h"
#include <string>
#include <map>

struct DeviceInfo
{
	std::string name;
	unsigned int type;
};

using DeviceMap = std::map<CAMERA, DeviceInfo>;

// Cn:枚举设备 | En:Enumerate Devices
DeviceMap EnumerateDevices(unsigned int type);

class VkCamera
{
public:
	explicit VkCamera(CAMERA handle = nullptr);
	~VkCamera();


	/*			--设备管理--			*/
	// Cn:连接设备 | En:Connect device
	int Connect(DisConnectCallbackFunc callback, void *context);

	// Cn:断开设备 | En:Disconnect device
	int Disconnect();

	// Cn:判断相机是否处于连接状态 | En:Is The Device Connected
	int IsDeviceConnected();
	
	// Cn:释放相机句柄 | En:Release camera handle
	int ReleaseCameraHandle();

	// Cn:释放设备资源 | En:Release device resources
	int ReleaseDeviceResources();

	/*			--图像采集与回调--			*/
	// Cn:注册抓图回调 | En:Register image callback
	int RegisterImageCallback(unsigned int channel, CallbackFunc func);

	// Cn:设置采集模式 | En:Set grab mode
	int SetAcquisitionMode(unsigned int grabMode);

	// Cn:相机输出图像是否支持可变帧长 | En:Camera support for variable frame
	int SupportVariableFrame(unsigned int isSupport);

	// Cn:开启抓图 | En:Start Grabbing
	int StartImageAcquisition(unsigned int timeout, unsigned int channel, void *context);

	// Cn:停止抓图 | En:Stop Grabbing
	int StopImageAcquisition(unsigned int channel);

	// Cn:在手动模式下获取图像 | En:Acquire images in manual mode
	int AcquireSingleFrame(unsigned int channel, unsigned int timeout, IMAGE* pImage);

	// Cn:获取图像信息 | En:Get image information
	int GetImageInfo(IMAGE image, PIMAGEINFO imageInfo);

	// Cn:获取相机帧统计信息 | En:Get camera frame statistics
	int GetFrameStatistics(STATISTICS statisticsInfo);

	// Cn:获取图像数据 | En:Get image data
	unsigned char *GetImageBuffer(IMAGE image);

	// Cn:获取图像数据长度 | En:Get image data length
	int GetBufferNeed(IMAGE image);

	// Cn:释放图像缓存 | En:Release image buffer
	void ReleaseImageBuffer(IMAGE image);

	// Cn:将图像格式转换为8位图像 | En:Convert the image format to 8-bit image
	IMAGE Unpack2Image8(IMAGE image);

	// Cn:判断相机是否正在输出图像 | En:Is camera grabbing
	int IsAcquisitionActive();

	// Cn:保存相机图像到文件 | En:Save image to file
	int SaveImage(IMAGE_META* pImage, SAVE_IMAGE_PARAM* pSaveParam, const char* pImagePath);


	/*			--参数获取与设置--			*/
	// Cn:保存相机配置文件到本地 | En:Save camera configuration file to local
	int ConfigSave(const char *cfgFileName);

	// Cn:加载相机配置文件到设备 | En:Load camera configuration file to device
	int ConfigLoad(const char *cfgFileName);

	// Cn:获取和设置Int型参数
	// En:Get Int type parameters
	int GetIntValue(const char *featureName, unsigned long long *featureValue);
	int SetIntValue(const char *featureName, unsigned long long featureValue);

	// Cn:获取和设置Enum型参数
	// En:Get Enum type parameters
	int GetEnumValue(const char *featureName, char *featureValue);
	int SetEnumValue(const char *featureName, const char* featureValue);

	// Cn:获取Enum型属性的有效值 | En:Get valid values for Enum type properties
	int GetFeatureEnumArray(const char *featureName, PENUMARRAY enumarray, int *enumCount);

	// Cn:获取和设置Double型参数
	// En:Get Double type parameters
	int GetDoubleValue(const char *featureName, double *featureValue);
	int SetDoubleValue(const char *featureName, double featureValue);

	// Cn:获取和设置Bool型参数
	// En:Get Bool type parameters
	int GetBoolValue(const char *featureName, unsigned int *featureValue);
	int SetBoolValue(const char *featureName, unsigned int featureValue);

	// Cn:获取和设置String型参数
	// En:Get String type parameters
	int GetStringValue(const char *featureName, unsigned char *featureValue);
	int SetStringValue(const char *featureName, const char *featureValue);

	// Cn:执行一次Command型命令
	// En:Execute Command once
	int ExecuteCommand(const char *featureName);

public:
	CAMERA GetHandle() { return m_cameraHandle; }
private:
	CAMERA m_cameraHandle;
};

#endif//_VK_CAMERA_H_
