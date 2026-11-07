#include <stdio.h>
#include <Windows.h>
#include <conio.h>
#include <string.h>
#include "VkApiDefination.h"

// 全局变量
bool g_bExit = false;
CAMERA g_cameraHandle = NULL;
CAMERA g_sg3000Handle = NULL;
CARD g_cardHandle = NULL;
CARD g_cardHandleSG = NULL;

void WaitForKeyPress(void)
{
	while (!_kbhit())
	{
		Sleep(10);
	}
	_getch();
}

void ImageCallback(IMAGE image, void* context)
{
	if (image)
	{
		IMAGE_INFO imageInfo;
		if (OK == VkGetImageInfo(image, &imageInfo))
		{
			printf("图像: %dx%d, 帧号[%lld]\n", imageInfo.width, imageInfo.height, imageInfo.blockId);
		}
		VkImageRelease(image);
	}
}

void DisconnectCallback(unsigned int errorCode, void* context)
{
	printf("设备断开！错误码: 0x%X\n", errorCode);
	g_bExit = true;
}

void CleanupResources()
{
	if (g_sg3000Handle != NULL)
	{
		VkStopGrab(g_sg3000Handle, 0);
		VkDisconnectCamera(g_sg3000Handle);
		VkCameraRelease(g_sg3000Handle);
		g_sg3000Handle = NULL;
	}

	if (g_cameraHandle != NULL)
	{
		VkDisconnectCamera(g_cameraHandle);
		VkCameraRelease(g_cameraHandle);
		g_cameraHandle = NULL;
	}

	if (g_cardHandle != NULL)
	{
		VkCardRelease(g_cardHandle);
		g_cardHandle = NULL;
	}

	if (g_cardHandleSG != NULL)
	{
		VkCardRelease(g_cardHandleSG);
		g_cardHandleSG = NULL;
	}

	VkDeviceRelease();
}

int main()
{
	int result = OK;
	printf("=== SG3000相机图像采集程序 ===\n");

	do
	{
		// 枚举父级设备
		// 针对CL自研采集卡使用，此处需要添加两个枚举协议类型：
		// DeviceTypeGencp：针对GenCP协议CL相机，该协议设备用于相机配参
		// DeviceTypeSG：针对CL采集卡设备，该协议设备用于采集卡配参、图像采集
		int cardnum = VkEnumCard(DeviceTypeGencp | DeviceTypeSG);
		if (cardnum <= 0)
		{
			printf("未找到设备！\n");
			result = ERROR_INVALIED_DEVICE_TYPE;
			break;
		}
		printf("找到 %d 个Card\n", cardnum);

		// 枚举所有设备
		typedef struct {
			char cardName[256];
			char deviceName[256];
			CARD cardHandle;
			bool isCamera;
		} DeviceInfo;

		DeviceInfo allDevices[100];
		int totalDeviceCount = 0;

		for (int cardIdx = 0; cardIdx < cardnum; cardIdx++)
		{
			char currentCardName[256] = { 0 };
			if (OK != VkGetCardName(cardIdx, currentCardName, sizeof(currentCardName)))
				continue;

			CARD currentCardHandle = VkGetCard(currentCardName);
			if (NULL == currentCardHandle)
				continue;

			int deviceCount = VkEnumCamera(currentCardHandle);
			for (int devIdx = 0; devIdx < deviceCount && totalDeviceCount < 100; devIdx++)
			{
				char deviceName[256] = { 0 };
				if (OK == VkGetCameraName(currentCardHandle, devIdx, deviceName, sizeof(deviceName)))
				{
					bool isCamera = (strstr(currentCardName, "GenCP") != NULL);
					strcpy_s(allDevices[totalDeviceCount].cardName, sizeof(allDevices[totalDeviceCount].cardName), currentCardName);
					strcpy_s(allDevices[totalDeviceCount].deviceName, sizeof(allDevices[totalDeviceCount].deviceName), deviceName);
					allDevices[totalDeviceCount].cardHandle = currentCardHandle;
					allDevices[totalDeviceCount].isCamera = isCamera;
					totalDeviceCount++;
				}
			}
		}

		if (totalDeviceCount == 0)
		{
			printf("未找到设备！\n");
			result = ERROR_INVALIED_DEVICE_TYPE;
			break;
		}

		// 显示设备列表
		// 该处为GenCP相机设备列表
		printf("\n相机设备:\n");
		// GenCP相机数量
		int cameraCount = 0;
		for (int i = 0; i < totalDeviceCount; i++)
		{
			if (allDevices[i].isCamera)
			{
				printf("  [%d] %s\n", i, allDevices[i].deviceName);
				cameraCount++;
			}
		}

		// 该处为CL采集卡设备列表
		printf("\n采集卡设备:\n");
		// CL采集卡设备数量
		int sg3000Count = 0;
		for (int i = 0; i < totalDeviceCount; i++)
		{
			if (!allDevices[i].isCamera)
			{
				printf("  [%d] %s\n", i, allDevices[i].deviceName);
				sg3000Count++;
			}
		}

		if (cameraCount == 0 || sg3000Count == 0)
		{
			printf("设备不完整！\n");
			result = ERROR_INVALIED_DEVICE_TYPE;
			break;
		}

		// 选择设备
		int selectedCameraIndex = -1, selectedSG3000Index = -1;
		printf("\n选择相机索引: ");
		scanf_s("%d", &selectedCameraIndex);
		printf("选择采集卡索引: ");
		scanf_s("%d", &selectedSG3000Index);

		if (selectedCameraIndex < 0 || selectedCameraIndex >= totalDeviceCount || !allDevices[selectedCameraIndex].isCamera ||
			selectedSG3000Index < 0 || selectedSG3000Index >= totalDeviceCount || allDevices[selectedSG3000Index].isCamera)
		{
			printf("设备索引无效！\n");
			result = ERROR_INVALID_INDEX;
			break;
		}

		// 获取设备句柄
		DeviceInfo* selectedCamera = &allDevices[selectedCameraIndex];
		DeviceInfo* selectedSG3000 = &allDevices[selectedSG3000Index];

		g_cardHandle = selectedCamera->cardHandle;
		g_cardHandleSG = selectedSG3000->cardHandle;


		// 获取 GenCP相机设备句柄，用于相机配参
		g_cameraHandle = VkGetCamera(g_cardHandle, selectedCamera->deviceName);
		// 获取 CL采集卡设备句柄，用于采集卡配参、图像采集
		g_sg3000Handle = VkGetCamera(g_cardHandleSG, selectedSG3000->deviceName);

		if (NULL == g_cameraHandle || NULL == g_sg3000Handle)
		{
			printf("获取设备句柄失败！\n");
			result = ERROR_INVALID_HANDLE;
			break;
		}


		// 连接设备

		// 连接GenCP相机设备(GenCP相机设备句柄)
		result = VkConnectCamera(g_sg3000Handle, DisconnectCallback, NULL);
		if (OK != result)
		{
			printf("连接采集卡失败！\n");
			break;
		}

		// 连接CL采集卡设备(CL采集卡设备句柄)
		result = VkConnectCamera(g_cameraHandle, DisconnectCallback, NULL);
		if (OK != result)
		{
			printf("连接相机失败！\n");
			break;
		}
		printf("设备连接成功\n");


		// 配置文件处理

		// GenCP相机设备将配置文件保存至本地（GenCP相机设备句柄）
		VkConfigSave(g_cameraHandle, "Camera_Config.cfg");
		// GenCP相机设备读取本地配置文件至设备（GenCP相机设备句柄）
		VkConfigLoad(g_cameraHandle, "Camera_Config.cfg");

		// CL采集卡设备将配置文件保存至本地（CL采集卡设备句柄）
		VkConfigSave(g_sg3000Handle, "SG3000_Config.cfg");
		// CL采集卡设备将配置文件保存至本地（CL采集卡设备句柄）
		VkConfigLoad(g_sg3000Handle, "SG3000_Config.cfg");


		// 注册图像采集回调函数（CL采集卡设备句柄）
		result = VkRegisterCallBack(g_sg3000Handle, 0, ImageCallback);
		if (OK != result)
		{
			printf("注册回调失败！\n");
			break;
		}

		// 启动图像采集（CL采集卡设备句柄）
		result = VkStartGrab(g_sg3000Handle, 500, 0, NULL);
		if (OK != result)
		{
			printf("启动采集失败！\n");
			break;
		}

		printf("图像采集已启动，按任意键停止...\n");
		WaitForKeyPress();


		// 停止图像采集（CL采集卡设备句柄）
		VkStopGrab(g_sg3000Handle, 0);

	} while (0);

	CleanupResources();
	printf("程序结束，按任意键退出...\n");
	WaitForKeyPress();
	return 0;
}