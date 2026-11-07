// VK SDK依赖结构体文件

#define IN			//输入参数
#define OUT			//输出参数

enum ERRORCODE												//错误码
{
	OK											= 0x0,					//OK
	ERROR_INVALID_HANDLES						= 0xFA000000,			//句柄错误
	ERROR_INVALID_DEVICE_TYPE					= 0xFA000001,			//设备类型错误
	ERROR_INVALID_ORDER						    = 0xFA000002,			//调用顺序错误
	ERROR_INVALID_INDEXS						= 0xFA000003,			//设备枚举编号越界
	ERROR_ALREADY_DISCONNECTED					= 0xFA000004,			//设备未连接
	ERROR_ALREDY_CONNECTED						= 0xFA000005,			//设备已连接
	ERROR_INVALID_PARA							= 0xFA000006,			//参数错误
	ERROR_GET_IMAGE_TIMEOUT						= 0xFA000007,			//获取图像超时无图像
	ERROR_NOT_FEATURE							= 0xFA000008,			//未找到相应属性
	ERROR_NOT_STREAM							= 0xFA000009,			//设备未开启数据流
	ERROR_DEVICE_NOT_SUPPORT_FEATURE			= 0xFA00000A,			//设备不支持参数设置
	ERROR_FEATURE_NOT_ACCESS					= 0xFA00000B,			//参数不可访问
	ERROR_CATCHED_GENI							= 0xFA00000C,			//参数异常
	ERROR_NO_REGISTER_CALLBACK                  = 0xFA00000D,           //开启图像采集前未注册回调函数
	ERR_BUFFER_OVERFLOW							= 0x80000001,			//内存池不足
	ERR_INVALID_INDEX							= 0x80000002,			//非法索引
	ERR_INSUFFICIENT_BUFFER						= 0x80000003,			//申请内存不足
	ERR_INVALD_PARAM							= 0x80000004,			//参数非法
	ERR_NOT_IMPLEMENTED							= 0x80000005,			//功能未实现
	ERR_FUNCTION_NOT_SUPPORTED					= 0x80000006,			//函数不支持
	ERR_CAMERA_NOT_CONNECTED					= 0x80000007,			//相机未连接
	ERR_CAMERA_ALREADY_CONNECTED				= 0x80000008,			//相机已连接
	ERR_CAEMRA_CONNECT_ERROR					= 0x80000009,			//相机连接出错
	ERR_CAMERA_NOT_STREAMING					= 0x8000000A,			//相机未开图像采集
	ERR_CAMERA_ALREADY_STREAMING				= 0x8000000B,			//相机已开启图像采集
	ERR_CAMERA_MESSAGE_NOT_SUPPORTED			= 0x8000000C,			//相机不支持消息通道
	ERR_CAMERA_MESSAGE_NOT_MONITORING			= 0x8000000D,			//相机消息通道未监听
	ERR_CAMERA_MESSAGE_ALREADY_MONITORING		= 0x8000000E,			//相机消息通道已监听
	ERR_CAMERA_POLLING_NOT_SUPPORTED			= 0x8000000F,			//相机Polling模式不支持
	ERR_CAMERA_POLLING_NOT_GOING				= 0x80000010,			//相机Polling未启动
	ERR_CAMERA_POLLING_ALREADY_RUNNING			= 0x80000011,			//相机Polling已运行				
	ERR_CAMERA_STREAM_CHANNEL_NOT_SUPPORTED		= 0x80000012,			//相机不支持此图像通道
	ERR_CAMERA_HEART_BEAT_ERROR					= 0x80000013,			//相机心跳超时
	ERR_CAMERA_XML_DATA_ERROR					= 0x80000014,			//相机描述文件数据错误
	ERR_INSUFFICIENT_DATA_RECEIVED				= 0x80000015,			//数据接收不完整
	ERR_INVALID_DATA_RECEIVED					= 0x80000016,			//接收数据非法
	ERR_GEV_ERR_UNKNOWN							= 0x80000017,			//网口相机未知错误
	ERR_CAMERA_URL_INVALID						= 0x80000018,			//相机描述文件路径错误
	ERR_TIME_OUT								= 0x80000019,			//超时
	ERR_SOCKET_API_ERROR						= 0x8000001A,			//Socket接口错误
	ERR_SOCKET_ALREADY_USED						= 0x8000001B,			//Socket已经使用
	ERR_COMM_PORT_ABORTED						= 0x8000001C,			//com口打开失败
	ERR_COMM_PORT_NOT_OPENED					= 0x8000001D,			//com口未打开
	ERR_GENICAM_INODE_NOT_FOUND					= 0x8000001E,			//无此节点
	ERR_IMAGE_INVALID_DATA						= 0x8000001F,			//图像数据错误
	ERR_IMAGE_IS_NULL							= 0x80000020,			//未采集到图像
	ERR_INVALID_GRAB_MODE						= 0x80000021,			//错误的采集模式

	ERR_CONFIG_LOAD_FAILED						= 0X80000022,	// 加载配置文件失败 文件不存在
	ERR_CONFIG_LOAD_HAVE_fAILNODE				= 0X80000023,	// 加载配置文件失败 获取文件节点失败
	ERR_CONFIG_SAVE_FAILED						= 0X80000024,	// 保存配置文件失败
	ERR_CONFIG_SAVE_NODEMAP_NULL				= 0X80000025,	// 保存配置文件失败 节点map为空
	ERR_CONFIG_SAVE_ROOTNODE_NULL				= 0X80000026,	// 保存配置文件失败 根节点root为空
	ERR_CONFIG_MODEL_ERROR						= 0X80000027,		// 配置文件类型不匹配
};

// 图像采集模式
typedef enum _GRABMODE
{
	GRAB_MODE_CALLBACK = 0,		//注册回调函数方式获取图像
	GRAB_MODE_MANUAL = 1,		//主动通过接口VkGetOneFrame获取图像
} GRAB_MODE;

// 内存池类型
typedef enum _PoolType
{
	BLOCK_MEMORY_POOL = 0x0,	//固定块内存池
	EASY_MEMORY_POOL = 0x1,		//动态申请空间内存池
}POOL_TYPE;

// 图像压缩类型
typedef enum _IMAGETYPE
{
	IMAGE_TYPE_IMAGE			= 1,		//IMAGE
	IMAGE_TYPE_RAWDATA			= 2,		//RawData
	IMAGE_TYPE_FILE				= 3,		//File
	IMAGE_TYPE_CHUNK			= 4,		//Chunk
	IMAGE_TYPE_JPEG				= 6,		//jpeg
	IMAGE_TYPE_JPEG2000			= 7,		//jpeg2000
	IMAGE_TYPE_H264				= 8,		//h264
	IMAGE_TYPE_MULTIZONE		= 9,		//multi zone
	IMAGE_TYPE_MULTIPART		= 10,		//multi part
	IMAGE_TYPE_DEVSPECISIC1		= 0x8001,	//自定义
} IMAGETYPE;
enum PIXEL_FORMAT											//图像像素格式
{
	PIXEL_FORMAT_MONO8				= 0x1080001,			//mono8
	PIXEL_FORMAT_MONO10				= 0X1100003,			//mono10
	PIXEL_FORMAT_MONO10_PACKED		= 0X10C0004,			//mono10packet
	PIXEL_FORMAT_MONO12				= 0X1100005,			//mono12
	PIXEL_FORMAT_MONO12_PACKED		= 0X10C0006,			//mono12packet
	PIXEL_FORMAT_MONO14				= 0X1100025,			//mono14
	PIXEL_FORMAT_MONO16				= 0X1100007,			//mono16
	PIXEL_FORMAT_RGB8				= 0X2180014,			//RGB8
	PIXEL_FORMAT_BGR8				= 0X2180015,			//BGR8
	PIXEL_FORMAT_RGB10_PACKED32		= 0X220001D,			//RGB_P32
	PIXEL_FORMAT_RGB10				= 0x2300018,			//RGB10
	PIXEL_FORMAT_BGR10				= 0x2300019,			//BGR10
	PIXEL_FORMAT_RGB12				= 0x230001A,			//RGB12
	PIXEL_FORMAT_BGR12				= 0x230001B,			//BGR12
	PIXEL_FORMAT_RGB10p32			= 0x220001D,			//RGB10p32
	PIXEL_FORMAT_RGBa8				= 0x02200016,			//RGBa8
	PIXEL_FORMAT_BAYER_GB8			= 0x108000A,			//BAYER_GB8
	PIXEL_FORMAT_BAYER_GR8			= 0x1080008,			//BAYER_GR8
	PIXEL_FORMAT_BAYER_RG8			= 0x1080009,			//BAYER_RG8
	PIXEL_FORMAT_BAYER_BG8			= 0x108000B,			//BAYER_BG8
	PIXEL_FORMAT_BAYER_GR10			= 0x110000C,			//BAYER_GR10
	PIXEL_FORMAT_BAYER_RG10			= 0x110000D,			//BAYER_RG10
	PIXEL_FORMAT_BAYER_GB10			= 0x110000E,			//BAYER_GB10
	PIXEL_FORMAT_BAYER_BG10			= 0x110000F,			//BAYER_BG10
	PIXEL_FORMAT_BAYER_GR12			= 0x1100010,			//BAYER_GR12
	PIXEL_FORMAT_BAYER_RG12			= 0x1100011,			//BAYER_RG12
	PIXEL_FORMAT_BAYER_GB12			= 0x1100012,			//BAYER_GB12
	PIXEL_FORMAT_BAYER_BG12			= 0x1100013,			//BAYER_BG12
	PIXEL_FORMAT_BAYER_GR10_PACKED	= 0x10C0026,			//BAYER_GR10_PACKED
	PIXEL_FORMAT_BAYER_RG10_PACKED	= 0x10C0027,			//BAYER_RG10_PACKED
	PIXEL_FORMAT_BAYER_GB10_PACKED	= 0x10C0028,			//BAYER_GB10_PACKED
	PIXEL_FORMAT_BAYER_BG10_PACKED	= 0x10C0029,			//BAYER_BG10_PACKED
	PIXEL_FORMAT_BAYER_GR12_PACKED	= 0x10C002A,			//BAYER_GR12_PACKED
	PIXEL_FORMAT_BAYER_RG12_PACKED	= 0x10C002B,			//BAYER_RG12_PACKED
	PIXEL_FORMAT_BAYER_GB12_PACKED	= 0x10C002C,			//BAYER_GB12_PACKED
	PIXEL_FORMAT_BAYER_BG12_PACKED	= 0x10C002D,			//BAYER_BG12_PACKED
	PIXEL_FORMAT_MONO8_64TAP		= 0x81080001,			//Motion Capture Camera Only
	PIXEL_FORMAT_MONO1_64TAP		= 0x81010001,			//Motion Capture Camera Only
	PIXEL_FORMAT_MONO8_48TAP		= 0x81080030,			//Motion Capture Camera Only
	PIXEL_FORMAT_MONO1_48TAP		= 0x81010030,			//Motion Capture Camera Only
	PIXEL_FORMAT_MONO8_32TAP		= 0x81080020,			//Motion Capture Camera Only
	PIXEL_FORMAT_MONO1_32TAP		= 0x81010020,			//Motion Capture Camera Only
	PIXEL_FORMAT_MONO8_16TAP		= 0x81080010,			//Motion Capture Camera Only
	PIXEL_FORMAT_MONO1_16TAP		= 0x81010010,			//Motion Capture Camera Only
	PIXEL_FORMAT_MONO10P			= 0x010A0046,			//mono10p
	PIXEL_FORMAT_MONO12P			= 0x010C0047,			//mono12p
	PIXEL_FORMAT_MONO14P			= 0x010E0104,			//mono14p
	PIXEL_FORMAT_RGB10P				= 0x021E005C,			//RGB10P
	PIXEL_FORMAT_RGB12P				= 0x0224005D,			//RGB12p
};
// 相机属性访问权限
enum EnumFeatureAccessMode
{
	ACCESS_NI,						//未实现
	ACCESS_NA,						//不可访问
	ACCESS_WO,						//只写
	ACCESS_RO,						//只读
	ACCESS_RW,						//读写
	ACCESS_UndefinedAccesMode,		//未定义
	ACCESS_CycleDetectAccesMode		//循环访问

};
// 相机属性可见能力
enum EnumFeatureVisibility
{
	Visible_Beginner			= 0,	//新手
	Visible_Expert				= 1,	//专家
	Visible_Guru				= 2,	//大师
	Visible_Invisible			= 3,	//不可见
	Visible_UndefinedVisibility = 99	//未定义
};
// 相机属性类型
typedef enum Feature_Type
{
	fIValue,		//!> IValue interface
	fIBase,			//!> IBase interface
	fIInteger,		//!> IInteger interface
	fIBoolean,		//!> IBoolean interface
	fICommand,		//!> ICommand interface
	fIFloat,		//!> IFloat interface
	fIString,		//!> IString interface
	fIRegister,		//!> IRegister interface
	fICategory,		//!> ICategory interface
	IEnumeration,	//!> IEnumeration interface
	IEnumEntry,		//!> IEnumEntry interface
	fIPort			//!> IPort interface
}FeatureType;
//支持的卡枚举类型
typedef enum Device_Type									
{
	DeviceTypeGencp			= 0x1,							// GenCp协议(CameraLink 相机)
	DeviceTypeU3V			= 0x2,							// U3V协议
	DeviceTypeDalsaCard		= 0x4,							// DALSA采集卡
	DeviceTypeGigE			= 0x8,							// 网卡Gige协议
	DeviceTypeSG			= 0x10,							// SG3000,XG, cxp
	GBoxCameraLinkCard		= 0x20,							// Gbox CameraLink
	GBoxGigeCard			= 0x40,							// GboxGe
	DeviceTypeLuster		= 0x80,							// LusterCamera
	DeviceTypeCXPCard		= 0x100,						// CXP采集卡
	DeviceTypeCLCard		= 0x200,						// CameraLink采集卡 
	DeviceTypeXGCard		= 0x400,						// XG Gige万兆网采集卡
	DeviceTypeAll			= 0xFFFF,						// 枚举全部
}DeviceType;

typedef struct IMAGE_INFO									//图像基本信息
{
	union
	{
		unsigned short fieldInfo;		//2Bytes			//版本号
		unsigned short version;
	};
	unsigned short imageType;			//2Bytes			//图像类型参考_IMAGETYPE，一般为IMAGE_TYPE_IMAGE
	unsigned int timeStampHigh;		//4Bytes				//时间戳
	unsigned int timeStampLow;		//4Bytes				//时间戳
	unsigned int format;			//4Bytes				//图像像素格式参考PIXEL_FORMAT
	union
	{
		unsigned int width;				//4Bytes			//图像宽度
		unsigned int payloadSizeHigh;	//4Bytes			
	};
	union
	{
		unsigned int height;			//4Bytes			//图像高度
		unsigned int payloadSizeLow;	//4Bytes
	};
	unsigned int startX;			//4Bytes				//图像横向偏移
	unsigned int startY;			//4Bytes				//图像纵向偏移
	unsigned int padding;			//4Bytes				//图像填充字节长度
	unsigned int actualHeight;		//4Bytes				//图像实际高度
	unsigned int packetsLost;		//4Bytes				//丢包数
	unsigned int packetsResend;		//4Bytes				//发起重传包数
	unsigned long long blockId;		//8Bytes				//帧ID
	unsigned short channel;          //2Bytes				//通道
	unsigned short flagExt;			//2Bytes				//包头标识位
	unsigned int reserved1;			//4Bytes				//预留
}*PIMAGEINFO;

// 图像数据信息
typedef struct Statistics_C
{
	unsigned int imagesCount;			// 图像计数
	unsigned int imagesLostCount;		// 图像丢失计数
	unsigned int segmentsResendCount;	// 分段重新发送计数
	unsigned int segmentsLostCount;		// 分段损失计数
	unsigned int lastTimestamp;			// 最后一次时间戳
	unsigned int imagesErrCount;		// 图像错误计数
	unsigned int nextBlockId;			// 下一个块Id
}*STATISTICS;

// 消息通道结构体
typedef struct 
{
	unsigned int eventId;			// 事件ID
	unsigned int channelIndex;		// 通道索引
	unsigned int blockIdHigh;		// 帧号高位
	unsigned int blockIdLow;		// 帧号低位
	unsigned int timeStampHigh;		// 时间戳高位
	unsigned int timeStampLow;		// 时间戳低位
}EVENT_ITEM;

typedef struct 
{
	char entry[256];
}ENUMARRAY, *PENUMARRAY;

typedef struct _FEATURETYPEINFO								//相机属性基本信息
{
	FeatureType type;										//相机属性类型
	int imin;												//fIInteger类型的最小值
	float dmin;												//fIFloat类型的最小值
	int imax;												//fIInteger类型最大值
	float dmax;												//fIFloat类型最大值
	int istep;												//fIInteger类型步长
	float dstep;											//fIFloat类型步长
	int stringLength;										//fIString类型可接受字符长度
}FEATURETYPEINFO, *PFEATURETYPEINFO;
