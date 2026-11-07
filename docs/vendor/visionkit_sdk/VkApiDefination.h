/**
 \file	VkApiDefination.h

 \brief	Declares the vk API defination class.

 对外C接口头文件类
 */

#pragma once
#include "VkApiStruct.h"
#ifdef __cplusplus
extern "C" {
#endif // __cplusplus
#ifdef __linux__
#define CAMERA_EXPORTS 
#else 
#define CAMERA_EXPORTS _declspec(dllexport)
#endif
typedef void* CARD;																//卡句柄类型
typedef void* CAMERA;															//相机句柄类型
typedef void* DEVICE;															//设备句柄类型，作为参数获取设置的类型
typedef void* NODE;																//相机属性节点
typedef unsigned int IMAGE;														//图像句柄类型
typedef void(*CallbackFunc)(IMAGE, void *);										//采流回调函数定义
typedef void(*DisConnectCallbackFunc)(unsigned int, void *);					//心跳超时间断开连接回调函数定义
typedef void(*MsgCallbackFunc)(const EVENT_ITEM *, const unsigned char *, unsigned int, void *); //消息监控

/*
*Description:获取SDK版本号
*Return:SDK版本号
*****/
CAMERA_EXPORTS const char* VkGetVersion();

/******************************卡相关***************************************/
/*
*Description:根据枚举类型获取卡的数量
*Para IN: device_type支持的类型,类型参考枚举DeviceType
*Return:枚举到的卡数量
*****/
CAMERA_EXPORTS int VkEnumCard(IN unsigned int device_type);

/*
*Description:根据卡的枚举编号获取卡名称
*Para IN:cardIdx 卡索引，从0开始计数
*Para OUT:pcCardName 获取的卡名称指针
*Para IN:CardNameLength 卡名称长度
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetCardName(IN int cardIdx, OUT char *pcCardName, IN int CardNameLength);

/*
*Description:根据根据卡名称获取卡句柄
*Para IN:pcCardName 卡名称，从VkCardNameGet中获取
*Return:成功卡句柄，失败返回NULL
*****/
CAMERA_EXPORTS CARD VkGetCard(IN const char *pcCardName);

/*
*Description:重新获取卡句柄，句柄计数会增加1，释放时需要多次释放
*Para IN:card 卡句柄
*Return:成功卡句柄，卡句柄计数增加1，释放时要多次释放
*/
CAMERA_EXPORTS CARD VkReGetCard(IN CARD card);

/*
*Description:释放卡句柄
*Para IN:card 卡句柄
*****/
CAMERA_EXPORTS void VkCardRelease(IN CARD card);

/*
*Description:获取设备协议类型，类型参考枚举DeviceType
*Para IN:card 卡句柄
*Return:设备协议类型, 类型参考枚举DeviceType
*****/
CAMERA_EXPORTS int VkGetCardType(IN CARD card);

/*Description:枚举卡下的所有相机，获取相机数量
*Para IN:card 卡句柄
*Return:卡下的相机数量
*****/
CAMERA_EXPORTS int VkEnumCamera(IN CARD card);

/*
*Description:根据卡句柄及卡枚举编号获取相机名称
*Para IN:card 卡句柄
*Para IN:card cameraIdx 相机索引，从0开始计数
*Para OUT:pcCameraName 相机名称指针
*Para IN:CameraNameLength 相机名称长度
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetCameraName(IN CARD card, IN int cameraIdx, OUT char *pcCameraName, IN int cameraNameLength);

/*
*Description:根据卡句柄及卡枚举编号获取相机句柄
*Para IN:card 卡句柄
*Para IN:pcCameraName 相机名称指针,从VkCameraNameGet中获取
*Return:成功相机句柄，失败返回NULL
*****/
CAMERA_EXPORTS CAMERA VkGetCamera(IN CARD card, IN const char *pcCameraName);

/******************************相机相关***************************************/
/*
*Description:根据类型获取枚举相机的数量
*Para IN: device_type支持的类型,类型参考枚举DeviceType
*Return:枚举到的相机数量
*****/
CAMERA_EXPORTS int VkEnumCameraByType(IN unsigned int device_type);

/*
*Description:根据相机的枚举编号获取相机名称
*Para IN:cardIdx 相机索引，从0开始计数
*Para OUT:pcCardName 获取的相机名称指针
*Para IN:CardNameLength 相机名称长度
*Return:错误返回ERRORCODE，成功返回相机名称实际长度
*****/
CAMERA_EXPORTS int VkGetCameraNameByIndex(IN int cameraIdx, OUT char *pcCameraName, IN int CameraNameLength);

/*
*Description:根据相机名称获取相机句柄
*Para IN:pcCameraName 相机名称，从VkGetCameraNameByIndex中获取
*Return:成功，返回相机句柄；失败，返回NULL
*****/
CAMERA_EXPORTS CAMERA VkGetCameraByName(IN const char *pcCameraName);

/*
*Description:重新获取相机句柄，句柄计数会增加1，释放时需要多次释放
*Para IN:camera 相机句柄
*Return:相机句柄,相机句柄计数增加1，释放时要多次释放
*****/
CAMERA_EXPORTS CAMERA VkReGetCamera(IN CAMERA camera);

/*
*Description:释放相机句柄
*Para IN:camera 相机句柄
*****/
CAMERA_EXPORTS void VkCameraRelease(IN CAMERA camera);

/*
*Description:获取相机设备协议类型，类型参考枚举DeviceType
*Para IN:camera 相机句柄
*Return:相机设备协议类型, 类型参考枚举DeviceType
*****/
CAMERA_EXPORTS int VkGetCameraType(IN CAMERA camera);

/*
*Description:连接相机
*Para IN:camera 相机句柄
*Para IN:callback 相机连接回调函数，网口相机心跳超时将进入此回调
*Para IN:context 上下文,在此函数输入的指针将作为callback的第二个参数
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkConnectCamera(IN CAMERA camera, IN DisConnectCallbackFunc callback,IN void *context);

/*
*Description:断开相机连接
*Para IN:camera 相机句柄
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkDisconnectCamera(IN CAMERA camera);

/*
*Description:判断相机是否连接
*Para IN:camera 相机句柄
*Return:连接返回OK，未连接返回ERROR_ALREADY_DISCONNECTED
*****/
CAMERA_EXPORTS int VkIsConnected(IN CAMERA camera);

/*
*Description:注册采流回调函数
*Para IN:camera 相机句柄
*Para IN:channel 相机输出图像通道，此值一般为0
*Para IN:func 相机输出图像回调函数
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkRegisterCallBack(IN CAMERA camera, IN unsigned int channel, IN CallbackFunc func);

/*
*Description:相机输出图像是否使用不完整帧
*Para IN:camera 相机句柄
*Para IN:isUse 1使用不完整帧，0不使用不完整帧
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkUnfullyFrameIsUse(IN CAMERA camera, IN unsigned int isUse);

/*
*Description:相机输出图像是否支持可变帧长
*Para IN:camera 相机句柄
*Para IN:isSupport 1支持可变帧长，0不支持可变帧长
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSupportVariableFrame(IN CAMERA camera, IN unsigned int isSupport);

/*
*Description:相机开启图像输出
*Para IN:camera 相机句柄
*Para IN:timeout 超时时间ms,接收不到图像数据的时间间隔，超时后将不完整的图像发给用户
*Para IN:channel 相机输出图像通道，此值一般为0
*Para IN:context 上下文，在此函数输入的指针将作为VkRegisterCallBack注册的func的第二个参数
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkStartGrab(IN CAMERA camera, IN unsigned int timeout, IN unsigned int channel, IN void *context);

/*
*Description:相机停止图像输出
*Para IN:camera 相机句柄
*Para IN:channel 相机输出图像通道，此值一般为0
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkStopGrab(IN CAMERA camera, IN unsigned int channel);

/*判断相机是否正在输出图像*/
/*
*Description:判断相机是否正在输出图像
*Para IN:image 图像句柄
*Return:采集图像返回OK，未采集图像返回ERR_CAMERA_NOT_STREAMING
*****/
CAMERA_EXPORTS int VkIsGrabbing(IN CAMERA camera);

/*
*Description:图像内存池初始化
*Para IN:camera 相机句柄
*Para IN:count 内存池缓存图像的数量
*Para IN:poolType 内存池类型，值参考枚举类型POOL_TYPE
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkMemoryPoolInit(IN CAMERA Camera, IN unsigned int count, IN unsigned short poolType);

/*
*Description:读取寄存器 注意端转换问题
*Para IN:camera 相机句柄
*Para OUT:pBuffer 读取寄存器内容
*Para IN:addr 读取寄存器地址
*Para IN:length 读取寄存器内容长度
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkReadAddressValue(IN CAMERA Camera, OUT void *pBuffer, IN unsigned long long addr, IN unsigned long long length);

/*
*Description:GenCP协议相机，使用最大波特率连接相机
*Para IN:camera 相机句柄
*Para IN:isMax 0为不使用最大波特率下载xml,1位使用使用最大波特率下载xml
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSetMaxBaudrateDownXmlSupport(IN CAMERA Camera, IN unsigned int isMax);


/*
*Description:写入寄存器 注意端转换问题
*Para IN:camera 相机句柄
*Para IN:pBuffer 写入寄存器内容
*Para IN:addr 写入寄存器地址
*Para IN:length 写入寄存器内容长度
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkWriteAddressValue(IN CAMERA Camera, IN void *pBuffer, IN unsigned long long addr, IN unsigned long long length);

/*
*Description:获取一帧图像
*Para IN:camera 相机句柄
*Para IN:timeout 超时时间ms,接收不到图像数据的时间间隔，超时后将不完整的图像发给用户
*Para IN:channel 相机输出图像通道，此值一般为0
*Return:成功返回图像句柄，失败返回NULL
*****/
CAMERA_EXPORTS IMAGE VkSnapImage(IN CAMERA camera, IN unsigned int timeout, IN unsigned int channel);

/*
*Description:开启消息监控
*Para IN:camera 相机句柄
*Para IN:func 相机连接回调函数，相机发出消息后进入此回调
*Para IN:context 上下文,在此函数输入的指针将作为callback的第二个参数
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkStartMsgMonitor(IN CAMERA camera, IN MsgCallbackFunc func, IN void *context);

/*
*Description:关闭消息监控
*Para IN:camera 相机句柄
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkStopMsgMonitor(IN CAMERA camera);

/*
*Description:设置采集模式
*Para IN:camera 相机句柄
*Para IN:grabMode 图像采集模式，参考GRAB_MODE
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSetGrabMode(IN CAMERA camera, IN unsigned int grabMode);

/*
*Description:设置GRAB_MODE_MANUAL模式下，保存图像队列长度
*Para IN:camera 相机句柄
*Para IN:channel 相机输出图像通道，此值一般为0
*Para IN:length 图像队列长度
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSetQueueLength(IN CAMERA camera, IN unsigned int channel, IN unsigned int length);

/*
*Description:设置GRAB_MODE_MANUAL模式下，清空图像队列
*Para IN:camera 相机句柄
*Para IN:channel 相机输出图像通道，此值一般为0
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkClearQueue(IN CAMERA camera, IN unsigned int channel);

/*
*Description:设置GRAB_MODE_MANUAL模式下，获取一帧图像
*Para IN:camera 相机句柄
*Para IN:channel 相机输出图像通道，此值一般为0
*Para IN:timeout 超时时间ms,接收不到图像数据的时间间隔，超时后将不完整的图像发给用户
*Para OUT:pImage 图像指针
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetOneFrame(IN CAMERA camera, IN unsigned int channel, IN unsigned int timeout, OUT IMAGE* pImage);

/*
*Description:获取相机MAC地址
*Para IN:camera 相机句柄
*Para OUT:pMac 相机mac
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS void VkGetCameraMac(IN CAMERA camera, OUT unsigned long long* mac);

/*
*Description:获取相机帧统计信息
*Para IN:camera 相机句柄
*Para OUT:imageInfo 图像基础信息
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetCameraStatistics(IN CAMERA camera, OUT STATISTICS statisticsInfo);

/******************************图像相关***************************************/
/*
*Description:根据图像句柄获取图像信息
*Para IN:image 图像句柄
*Para OUT:imageInfo 图像基础信息
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetImageInfo(IN IMAGE image, OUT PIMAGEINFO imageInfo);

/*
*Description:根据图像句柄获取图像数据
*Para IN:image 图像句柄
*Return:图像数据首地址
*****/
CAMERA_EXPORTS unsigned char *VkGetImageBuff(IN IMAGE Image);

/*
*Description:根据图像信息获取图像数据长度
*Para IN:image 图像句柄
*Return:图像数据需要占用bytes
*****/
CAMERA_EXPORTS int VkGetBuffNeed(IN IMAGE image);

/*
*Description:重新获取图像句柄，句柄计数会增加1，释放时需要多次释放
*Para IN:image 图像句柄
*Return:图像句柄,句柄计数会增加1，释放时需要多次释放
*****/
CAMERA_EXPORTS IMAGE VkReGetImage(IN IMAGE image);

/*
*Description:释放图像句柄
*Para IN:image 图像句柄
*****/
CAMERA_EXPORTS void VkImageRelease(IN IMAGE image);

/*
*Description:将Unpack图像格式转换为8位图像
*Para IN:image 图像句柄
*Return:解码后的图像句柄
*****/
CAMERA_EXPORTS IMAGE VkUnpack2Image8(IN IMAGE image);

/*
*Description:面阵bayer 图像转换为RGB8图像
*Para IN:image 图像句柄
*Return:解码后的图像句柄
*****/
CAMERA_EXPORTS IMAGE VkAreaBayerRGDecode(IN IMAGE image);

/*
*Description:线阵bayer RG图像转换为RGB8图像
*Para IN:isMirror	是否镜像 1是镜像 0是非镜像
*Para IN:image 图像句柄
*Return:解码后的图像句柄
*****/
CAMERA_EXPORTS IMAGE VkLineBayerRGDecode(IN int isMirror, IN IMAGE image);

/*
*Description:RGB图像Binning2X1功能,分辨率宽为原始的一半
*Para IN:image 图像句柄
*Return:Binning过滤后图像
*****/
CAMERA_EXPORTS IMAGE VkBinning2X1(IN IMAGE image);

/*
*Description:RGB图像Binning2X2功能,分辨率宽及高为原始的一半
*Para IN:image 图像句柄
*Return:Binning过滤后图像
*****/
CAMERA_EXPORTS IMAGE VkBinning2X2(IN IMAGE image);

/******************************设备（相机&&卡）相关***************************************/
/*
*Description:设备资源释放
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkDeviceRelease();

/*
*Description:存储设备的当前配置至本地文件;必须保证相机VkConnectCamera成功后，VkStartGrab前
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:cfgFileName 本地文件所在路径与名称，例如C:/Users/administrator/Desktop/cfgfilename.cfg
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkConfigSave(IN DEVICE dev, IN const char *cfgFileName);

/*
*Description:加载当前设置至设备;必须保证相机VkConnectCamera成功后，VkStartGrab前
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:cfgFileName 本地文件所在路径与名称，例如C:/Users/administrator/Desktop/cfgfilename.cfg
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkConfigLoad(IN DEVICE dev, IN const char *cfgFileName);

/*
*Description:获取设备参数的类型、取值范围、步长等信息;		
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:featureType 参数的类型及参数的取值范围、步长
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureInfo( IN DEVICE dev, IN const char *featureName, OUT PFEATURETYPEINFO featureType);

/*
*Description:获取设备参数的类型;
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:featureType 参数的类型参考枚举FeatureType
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureType(IN DEVICE dev, IN const char *featureName, OUT unsigned int* featureType);

/*
*Description:获取设备参数的访问权限
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:accessMode 参数的访问权限枚举值参考EnumFeatureAccessMode
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureAccessMode(IN DEVICE dev, IN const char *featureName, OUT unsigned int* accessMode);

/*
*Description:获取设备参数Integer类型取值范围
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:min 参数最小值
*Para OUT:max 参数最大值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureIntRange(IN DEVICE dev, IN const char *featureName, OUT unsigned long long *min, OUT unsigned long long *max);

/*
*Description:获取设备参数Integer类型步长
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:step 参数步长
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureIntStep(IN DEVICE dev, IN const char *featureName, OUT unsigned long long *step);

/*
*Description:获取设备参数Integer类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureInt(IN DEVICE dev, IN const char *featureName, OUT unsigned long long *featureValue);

/*
*Description:设置设备参数Integer类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para IN:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSetFeatureInt(IN DEVICE dev, IN const char *featureName, IN unsigned long long featureValue);

/*
*Description:获取设备参数Boolean类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureBoolean(IN DEVICE dev, IN const char *featureName, OUT unsigned int *featureValue);

/*
*Description:设置设备参数Boolean类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para IN:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSetFeatureBoolean(IN DEVICE dev, IN const char *featureName, IN unsigned int featureValue);

/*
*Description:获取设备参数Double类型取值范围
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:min 参数最小值
*Para OUT:max 参数最大值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureDoubleRange(IN DEVICE dev, IN const char *featureName, OUT double *min, OUT double *max);

/*
*Description:获取设备参数Double类型步长
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:step 参数步长
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureDoubleStep(IN DEVICE dev, IN const char *featureName, OUT double *step);

/*
*Description:获取设备参数Double类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureDouble(IN DEVICE dev, IN const char *featureName, OUT double *featureValue);

/*
*Description:设置设备参数Double类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para IN:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSetFeatureDouble(IN DEVICE dev, IN const char *featureName, IN double featureValue);

/*
*Description:获取设备参数String类型属性值最大长度
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:length 参数值最大长度
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureMaxLengthString(IN DEVICE dev, IN const char *featureName, OUT unsigned long long *length);

/*
*Description:获取设备参数String类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureString(IN DEVICE dev, IN const char *featureName, OUT unsigned char *featureValue);

/*
*Description:设置设备参数String类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para IN:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSetFeatureString(IN DEVICE dev, IN const char *featureName, IN const char *featureValue);

/*
*Description:获取设备参数Enumeration类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureEnum(IN DEVICE dev, IN const char *featureName, OUT char *featureValue);

/*
*Description:设置设备参数Enumeration类型属性值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para IN:featureValue 参数值
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkSetFeatureEnum(IN DEVICE dev, IN const char *featureName, IN const char* featureValue);

/*
*Description:获取设备参数Enumeration类型属性有效值
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Para OUT:enumarray 参数有效值数组
*Para OUT:enumCount 参数有效值数量
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetFeatureEnumArray(IN DEVICE dev, IN const char *featureName, OUT PENUMARRAY enumarray, OUT int *enumCount);

/*
*Description:执行Command类型参数
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Para IN:featureName 参数名称
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkExecuteFeatureCommand(IN DEVICE dev, IN const char *featureName);

/*
*Description:获取设备功能描述文件根节点句柄
*Para IN:dev 设备句柄，参数可以传入CAMERA或CARD类型句柄
*Return:参数根节点句柄
*****/
CAMERA_EXPORTS NODE VkGetRootNode(IN DEVICE dev);

/*
*Description:获取子节点的句柄列表及子节点数量,NodeList是空只获取子节点数量
*Para IN:node 参数节点句柄
*Para OUT:nodeList 子节点数组
*Para OUT:nodeListSize 子节点数量
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetNodeChildren(IN NODE node, OUT NODE *nodeList, OUT unsigned int *nodeListSize);

/*
*Description:获取节点描述名称
*Para IN:node 参数节点句柄
*Para OUT:displayName 参数节点描述名称
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetNodeDisplayName(IN NODE node, OUT char *displayName);

/*
*Description:获取节点名称
*Para IN:node 参数节点句柄
*Para OUT:pName 参数节点名称
*Return:ERRORCODE，成功返回OK
*****/
CAMERA_EXPORTS int VkGetNodeName(IN NODE node, OUT char *nName);

/*
*Description:判断节点是否为属性节点
*Para IN:node 参数节点句柄
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkIsFeatureNode(IN NODE node);

/*
*Description:获取节点类型
*Para IN:node 参数节点句柄
*Para IN:type 参数节点类型，值参考FeatureType
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeType(IN NODE node,OUT int *type);

/*
*Description:获取节点的访问权限
*Para IN:node 参数节点句柄
*Para OUT:accessMode 参数节点访问权限，值参考EnumFeatureAccessMode
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeAccessMode(IN NODE node, OUT int *accessMode);

/*
*Description:获取节点参数的可见性
*Para IN:node 参数节点句柄
*Para OUT:visibility 参数节点可见性
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeVisibility(IN NODE node, OUT int *visibility);

/*
*Description:获取Int类型节点参数范围
*Para IN:node 参数节点句柄
*Para OUT:min 参数节点最小值
*Para OUT:max 参数节点最大值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeRangeInt(IN NODE node,OUT long long *min, OUT long long *max);

/*
*Description:获取fIInteger类型节点参数步长
*Para IN:node 参数节点句柄
*Para OUT:step 参数节点步长
*Return:正确返回OK,错误返回ERRORCODE
*****/
CAMERA_EXPORTS int VkGetNodeStepInt(IN NODE node, OUT unsigned int *step);

/*
*Description:获取Float类型节点参数范围
*Para IN:node 参数节点句柄
*Para OUT:min 参数节点最小值
*Para OUT:max 参数节点最大值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeRangeDouble(IN NODE node, OUT double *min, OUT double *max);

/*
*Description:获取fIFloat类型节点参数步长
*Para IN:node 参数节点句柄
*Para OUT:step 参数节点步长
*Return:正确返回OK,错误返回ERRORCODE
*****/
CAMERA_EXPORTS int VkGetNodeStepDouble(IN NODE node, OUT double *step);

/*
*Description:获取String类型节点支持最大长度
*Para IN:node 参数节点句柄
*Para OUT:maxLength 参数节点值最大字符长度
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeMaxLengthString(IN NODE node, OUT long long *maxLength);

/*
*Description:获取Enum类型节点成员节点列表及数量
*Para IN:node 参数节点句柄
*Para OUT:nodeList 参数节点子节点数组
*Para OUT:size 参数节点子节点数量
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeEnumEntrys(IN NODE node, OUT NODE *nodeList, OUT unsigned int *size);

/*
*Description:获取Enum类型的成员节点名称
*Para IN:node 参数节点句柄,句柄从VkGetNodeEnumEntrys的nodeList中获取
*Para OUT:symbolic 参数节点名称
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeEnumEntrySymbolic(IN NODE node, OUT char *symbolic);

/*
*Description:获取Enum类型的子节点的节点值
*Para IN:node 参数节点句柄,句柄从VkGetNodeEnumEntrys的nodeList中获取
*Para OUT:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeEnumEntryValue(IN NODE node, OUT unsigned int *value);

/*
*Description:获取(VkGetNodeType 输出的值等于fIInteger)节点值
*Para IN:node 参数节点句柄
*Para OUT:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeValueInt(IN NODE Node, OUT unsigned long long *featureValue);

/*
*Description:设置(VkGetNodeType 输出的值等于fIInteger)节点值
*Para IN:node 参数节点句柄
*Para IN:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkSetNodeValueInt(IN NODE Node, IN unsigned long long featureValue);

/*
*Description:获取(VkGetNodeType 输出的值等于fIFloat)节点值
*Para IN:node 参数节点句柄
*Para OUT:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeValueDouble(IN NODE Node, OUT double *featureValue);

/*
*Description:设置(VkGetNodeType 输出的值等于fIFloat)节点值
*Para IN:node 参数节点句柄
*Para IN:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkSetNodeValueDouble(IN NODE Node, IN double featureValue);

/*
*Description:获取(VkGetNodeType 输出的值等于fIBoolean)节点值
*Para IN:node 参数节点句柄
*Para OUT:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeValueBoolean(IN NODE Node, OUT unsigned int *featureValue);

/*
*Description:设置(VkGetNodeType 输出的值等于fIBoolean)节点值
*Para IN:node 参数节点句柄
*Para IN:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkSetNodeValueBoolean(IN NODE Node, IN unsigned int featureValue);

/*
*Description:获取(VkGetNodeType 输出的值等于fIString)节点值
*Para IN:node 参数节点句柄
*Para OUT:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeValueString(IN NODE Node, OUT unsigned char *featureValue);

/*
*Description:设置(VkGetNodeType 输出的值等于fIString)节点值
*Para IN:node 参数节点句柄
*Para IN:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkSetNodeValueString(IN NODE Node, IN const char *featureValue);

/*
*Description:获取(VkGetNodeType 输出的值等于IEnumeration)节点值
*Para IN:node 参数节点句柄
*Para OUT:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkGetNodeValueEnum(IN NODE Node, OUT unsigned int *featureValue);

/*
*Description:设置(VkGetNodeType 输出的值等于IEnumeration)节点值
*Para IN:node 参数节点句柄
*Para IN:value 参数节点值
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkSetNodeValueEnum(IN NODE Node, IN unsigned int featureValue);

/*
*Description:执行(VkGetNodeType 输出的值等于fICommand)节点
*Para IN:node 参数节点句柄
*Return:ERRORCODE，是参数节点返回OK
*****/
CAMERA_EXPORTS int VkExecuteNodeValueCommand(IN NODE Node);


#ifdef __cplusplus
}
#endif // __cplusplus