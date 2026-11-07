#ifndef _VK_IMAGE_IO_H_
#define _VK_IMAGE_IO_H_

#include <stddef.h>  // 包含NULL定义

// DLL导出宏定义
#ifdef VK_SDK_ALG_EXPORTS
#define VK_SDK_ALG_API __declspec(dllexport)
#else
#define VK_SDK_ALG_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif


#define IN          //输入参数
#define OUT         //输出参数

// VkImageIO错误码枚举（仅保留实际使用的错误码）
enum VK_IMAGEIO_ERR
{
	// 成功状态
	ERR_OK                          = 0x00000000,                  // 成功

	// 参数错误类 (0x8000001x)
	ERR_NULL_POINTER                = 0x80000001,                  // 空指针参数
	ERR_INVALID_IMAGE_DATA          = 0x80000002,                  // 图像数据无效（宽高为0或缓冲区为空）
	ERR_INVALID_PIXEL_FORMAT        = 0x80000003,                  // 不支持的像素格式
	ERR_INVALID_SAVE_FORMAT         = 0x80000004,                  // 不支持的保存格式

	// 内存错误类 (0x8000002x)
	ERR_INSUFFICIENT_MEMORY         = 0x80000020,                  // 内存分配失败

	// 文件操作错误类 (0x8000003x)
	ERR_FILE_OPEN_FAILED            = 0x80000030,                  // 文件打开失败
	ERR_FILE_WRITE_FAILED           = 0x80000031,                  // 文件写入失败

	// 图像处理错误类 (0x8000004x)
	ERR_IMAGE_ENCODE_FAILED         = 0x80000040,                  // 图像编码失败
	ERR_OPENCV_EXCEPTION            = 0x80000041,                  // OpenCV异常

	// 系统错误类 (0x8000005x)
	ERR_SYSTEM_ERROR                = 0x80000050,                  // 系统错误
	ERR_UNKNOWN_EXCEPTION           = 0x80000051,                  // 未知异常

};

/************************************************************************/
/*     GigE Vision (2.0.03) PIXEL FORMATS                               */
/************************************************************************/

// 指示像素是单色还是彩色
#define GVSP_PIX_MONO                                0x01000000
#define GVSP_PIX_COLOR                               0x02000000
#define GVSP_PIX_CUSTOM                              0x80000000
#define GVSP_PIX_COLOR_MASK                          0xFF000000

// 指示像素占用的有效位数（包括填充）
#define PIXEL_BIT_COUNT(n)                           ((n) << 16)

// 图像元数据像素格式枚举
enum IMAGE_META_FORMAT
{
	// 单色缓冲格式定义
	IMAGE_META_FORMAT_MONO1p            = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(1) | 0x0037),     // mono1p
	IMAGE_META_FORMAT_MONO2p            = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(2) | 0x0038),     // mono2p
	IMAGE_META_FORMAT_MONO4p            = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(4) | 0x0039),     // mono4p
	IMAGE_META_FORMAT_MONO8             = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x0001),     // mono8
	IMAGE_META_FORMAT_MONO10            = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0003),    // mono10
	IMAGE_META_FORMAT_MONO10_PACKED     = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x0004),    // mono10packet
	IMAGE_META_FORMAT_MONO12            = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0005),    // mono12
	IMAGE_META_FORMAT_MONO12_PACKED     = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x0006),    // mono12packet
	IMAGE_META_FORMAT_MONO14            = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0025),    // mono14
	IMAGE_META_FORMAT_MONO16            = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0007),    // mono16

	// Bayer缓冲格式定义
	IMAGE_META_FORMAT_BAYER_GR8         = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x0008),     // BAYER_GR8
	IMAGE_META_FORMAT_BAYER_RG8         = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x0009),     // BAYER_RG8
	IMAGE_META_FORMAT_BAYER_GB8         = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x000A),     // BAYER_GB8
	IMAGE_META_FORMAT_BAYER_BG8         = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x000B),     // BAYER_BG8
	IMAGE_META_FORMAT_BAYER_RBGG8       = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x0046),     // BAYER_RBGG8
	IMAGE_META_FORMAT_BAYER_GR10        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x000C),    // BAYER_GR10
	IMAGE_META_FORMAT_BAYER_RG10        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x000D),    // BAYER_RG10
	IMAGE_META_FORMAT_BAYER_GB10        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x000E),    // BAYER_GB10
	IMAGE_META_FORMAT_BAYER_BG10        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x000F),    // BAYER_BG10
	IMAGE_META_FORMAT_BAYER_GR12        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0010),    // BAYER_GR12
	IMAGE_META_FORMAT_BAYER_RG12        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0011),    // BAYER_RG12
	IMAGE_META_FORMAT_BAYER_GB12        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0012),    // BAYER_GB12
	IMAGE_META_FORMAT_BAYER_BG12        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0013),    // BAYER_BG12
	IMAGE_META_FORMAT_BAYER_GR10_PACKED = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x0026),    // BAYER_GR10_PACKED
	IMAGE_META_FORMAT_BAYER_RG10_PACKED = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x0027),    // BAYER_RG10_PACKED
	IMAGE_META_FORMAT_BAYER_GB10_PACKED = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x0028),    // BAYER_GB10_PACKED
	IMAGE_META_FORMAT_BAYER_BG10_PACKED = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x0029),    // BAYER_BG10_PACKED
	IMAGE_META_FORMAT_BAYER_GR12_PACKED = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x002A),    // BAYER_GR12_PACKED
	IMAGE_META_FORMAT_BAYER_RG12_PACKED = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x002B),    // BAYER_RG12_PACKED
	IMAGE_META_FORMAT_BAYER_GB12_PACKED = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x002C),    // BAYER_GB12_PACKED
	IMAGE_META_FORMAT_BAYER_BG12_PACKED = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x002D),    // BAYER_BG12_PACKED
	IMAGE_META_FORMAT_BAYER_GR16        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x002E),    // BAYER_GR16
	IMAGE_META_FORMAT_BAYER_RG16        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x002F),    // BAYER_RG16
	IMAGE_META_FORMAT_BAYER_GB16        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0030),    // BAYER_GB16
	IMAGE_META_FORMAT_BAYER_BG16        = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(16) | 0x0031),    // BAYER_BG16

	// RGB打包缓冲格式定义
	IMAGE_META_FORMAT_RGB8              = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(24) | 0x0014),   // RGB8
	IMAGE_META_FORMAT_BGR8              = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(24) | 0x0015),   // BGR8
	IMAGE_META_FORMAT_RGBa8             = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(32) | 0x0016),   // RGBa8
	IMAGE_META_FORMAT_RGB10_PACKED32    = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(32) | 0x001D),   // RGB_P32
	IMAGE_META_FORMAT_RGB10             = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(48) | 0x0018),   // RGB10
	IMAGE_META_FORMAT_BGR10             = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(48) | 0x0019),   // BGR10
	IMAGE_META_FORMAT_RGB12             = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(48) | 0x001A),   // RGB12
	IMAGE_META_FORMAT_BGR12             = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(48) | 0x001B),   // BGR12
	IMAGE_META_FORMAT_RGB10p32          = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(48) | 0x001D),   // RGB10p32

	// 运动捕捉相机专用格式（自定义GVSP格式）
	IMAGE_META_FORMAT_MONO8_64TAP       = (GVSP_PIX_CUSTOM | GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x0001),  // Motion Capture Camera Only
	IMAGE_META_FORMAT_MONO1_64TAP       = (GVSP_PIX_CUSTOM | GVSP_PIX_MONO | PIXEL_BIT_COUNT(1) | 0x0001),  // Motion Capture Camera Only
	IMAGE_META_FORMAT_MONO8_48TAP       = (GVSP_PIX_CUSTOM | GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x0030),  // Motion Capture Camera Only
	IMAGE_META_FORMAT_MONO1_48TAP       = (GVSP_PIX_CUSTOM | GVSP_PIX_MONO | PIXEL_BIT_COUNT(1) | 0x0030),  // Motion Capture Camera Only
	IMAGE_META_FORMAT_MONO8_32TAP       = (GVSP_PIX_CUSTOM | GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x0020),  // Motion Capture Camera Only
	IMAGE_META_FORMAT_MONO1_32TAP       = (GVSP_PIX_CUSTOM | GVSP_PIX_MONO | PIXEL_BIT_COUNT(1) | 0x0020),  // Motion Capture Camera Only
	IMAGE_META_FORMAT_MONO8_16TAP       = (GVSP_PIX_CUSTOM | GVSP_PIX_MONO | PIXEL_BIT_COUNT(8) | 0x0010),  // Motion Capture Camera Only
	IMAGE_META_FORMAT_MONO1_16TAP       = (GVSP_PIX_CUSTOM | GVSP_PIX_MONO | PIXEL_BIT_COUNT(1) | 0x0010),  // Motion Capture Camera Only

	// 其他格式
	IMAGE_META_FORMAT_MONO10P           = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(10) | 0x0046),      // mono10p
	IMAGE_META_FORMAT_MONO12P           = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(12) | 0x0047),      // mono12p
	IMAGE_META_FORMAT_MONO14P           = (GVSP_PIX_MONO | PIXEL_BIT_COUNT(14) | 0x0104),      // mono14p
	IMAGE_META_FORMAT_RGB10P            = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(30) | 0x005C),     // RGB10P
	IMAGE_META_FORMAT_RGB12P            = (GVSP_PIX_COLOR | PIXEL_BIT_COUNT(36) | 0x005D),     // RGB12p
};

// 用于存储图像的基本信息和像素数据
typedef struct _IMAGE_META_
{
	unsigned int			nWidth;				// [IN]     图像宽度（像素单位）
	unsigned int			nHeight;			// [IN]     图像高度（像素单位）
	enum IMAGE_META_FORMAT	enPixelFormat;		// [IN]     像素格式（如MONO8、RGB8等）

	unsigned char*			pImageBuffer;		// [IN]     指向图像像素数据的缓冲区指针
	unsigned int			nImageBufferLength;	// [IN]     图像数据实际长度（字节数）

	_IMAGE_META_()
	{
		nWidth = 0;
		nHeight = 0;
		enPixelFormat = IMAGE_META_FORMAT_MONO8;
		pImageBuffer = NULL;
		nImageBufferLength = 0;
	}
} IMAGE_META;

// 图像保存类型枚举
enum SAVE_IMAGE_TYPE
{
	Image_Undefined = 0,
	Image_Raw = 1,
	Image_Bmp = 2,
	Image_Png = 3,
	Image_Jpeg = 4,
	Image_Tif = 5
};

// 保存图片所需参数结构体
typedef struct _SAVE_IMAGE_PARAM_
{
	enum SAVE_IMAGE_TYPE enImageType;	   // [IN]     输入图片格式
	unsigned int nQuality;                 // [IN]     JPG编码质量(50-99]，其它格式无效
	int iMethodValue;                      // [IN]     插值方法 0-快速 1-均衡（其它值默认为均衡） 2-最优 3-最优+

	_SAVE_IMAGE_PARAM_()
	{
		enImageType = Image_Undefined;
		nQuality = 80;
		iMethodValue = 1;
	}
} SAVE_IMAGE_PARAM;

/**
*  @brief  保存图像到文件
*  @param  [IN]  pImageMeta      图像元数据指针
*  @param  [IN]  pSaveParam      保存图片所需参数结构体
*  @param  [IN]  pImagePath      存图路径
*  @return 成功，返回OK；错误，返回VK_ERR错误码
*/
VK_SDK_ALG_API int VkWriteImage(IN IMAGE_META* pImageMeta, IN SAVE_IMAGE_PARAM* pSaveParam, IN const char* pImagePath);

#ifdef __cplusplus
}
#endif

#endif // _VK_IMAGE_IO_H_
