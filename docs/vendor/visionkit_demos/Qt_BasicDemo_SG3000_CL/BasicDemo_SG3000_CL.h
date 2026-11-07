#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_BasicDemo_SG3000_CL.h"
#include "VkCamera.h"

class BasicDemo_SG3000_CL : public QMainWindow
{
    Q_OBJECT
		
public:
	explicit BasicDemo_SG3000_CL(QWidget *parent = Q_NULLPTR);
	~BasicDemo_SG3000_CL() override;

	VkCamera* getDevController() const { return m_pSGCard; }		// Cn:获取当前采集卡控制器实例 | En: Get Active SG Card Controller Instance

signals:
	void imageReceived(const QImage& image);							// Cn:图像更新信号 | En:Image Update Signal
	void statisticsInfoUpdated(int imgCnt, int imgErrCnt,
		int imgLostCnt, int segmentsLostCnt, int segmentsResendCnt);	// Cn:图像统计信息更新信号 | En:Image Statistics Update Signal
	void heartbeatTimeout();											// Cn:相机心跳超时信号 | En:Camera Heartbeat Timeout

	public slots:
	/* Cn:设备管理 | En:Device Management */
	void OnEnumButtonClicked();					                // Cn:枚举所有可用采集卡与相机  | En:Enumerate All Available SG Cards and Cameras
	void OnSGOpenButtonClicked();				                // Cn:连接选择的采集卡 | En:Connect SG Card
	void OnSGCloseButtonClicked();				                // Cn:关闭选择的采集卡 | En:Disconnect SG Card
	void OnSGListBoxChanged(QString sgCardName);		        // Cn:通过名称创建新采集卡实例 | En:Create a New SG Card Instance by Name
	void OnCamOpenButtonClicked();				                // Cn:连接选择的相机 | En:Connect Camera
	void OnCamCloseButtonClicked();				                // Cn:关闭选择的相机 | En:Disconnect Camera
	void OnCamListBoxChanged(QString cameraName);		        // Cn:通过名称创建新相机实例 | En:Create a New Camera Instance by Name

																/* Cn:图像采集与保存 | En:Image Acquisition and Save */
	void OnStartGrabButtonClicked();			                // Cn:开始采集图像 | En:Start Grabbing
	void OnStopGrabButtonClicked();				                // Cn:停止采集图像 | En:Stop Grabbing
	void OnSaveRawButtonClicked();				                // Cn:保存Raw格式图像 | En:Save Raw	Format Image
	void OnSaveBmpButtonClicked();				                // Cn:保存Bmp格式图像 | En:Save Bmp	Format Image
	void OnSaveJpegButtonClicked();				                // Cn:保存Jpeg格式图像 | En:Save Jpeg Format Image
	void OnSaveTifButtonClicked();				                // Cn:保存Tif格式图像 | En:Save Tif Format Image
	void OnSavePngButtonClicked();				                // Cn:保存Png格式图像 | En:Save Png	Format Image

																/* Cn:参数获取与设置（采集卡） | En:Parameters Get and Set (SG Card) */
	void OnLinkConfigBoxChanged(QString featureName);			// Cn:设置Camera Link Config参数 | En:Set Camera Link Config Parameter
	void OnColorTypeBoxChanged(QString featureName);			// Cn:设置Camera Color Type参数 | En:Set Camera Color Type Parameter
	void OnPixelDepthBoxChanged(QString featureName);			// Cn:设置Camera Pixel Depth参数 | En:Set Camera Pixel Depth Parameter
	void OnScanTypeBoxChanged(QString featureName);				// Cn:设置Camera Scan Type参数 | En:Set Camera Scan Type Parameter
	void OnTapGeometryBoxChanged(QString featureName);			// Cn:设置Device Tap Geometry参数 | En:Set Device Tap Geometry Parameter
	void OnHorizontalEditPressed();								// Cn:设置Horizontal Active参数 | En:Set Horizontal Active Parameter
	void OnVerticalEditPressed();								// Cn:设置Vertical Active参数 | En:Set Vertical Active Parameter
	void OnCC1BoxChanged(QString featureName);		   			// Cn:设置CC1参数 | En:Set CC1 Parameter
	void OnLineTriggerEnableBoxChanged(QString featureName);   	// Cn:设置Line Trigger Enable参数 | En:Set Line Trigger Enable Parameter
	void OnLineTriggerSourceBoxChanged(QString featureName);   	// Cn:设置Line Trigger Source参数 | En:Set Line Trigger Source Parameter
	void OnLineTriggerRateEditPressed();					  	// Cn:设置Line Internal Trigger Rate参数 | En:Set Line Internal Trigger Rate Parameter
	void OnLineTriggerSoftwareButtonClicked();				  	// Cn:执行Line Trigger Software命令 | En:Execute Line Trigger Software Command
	void OnFrameTriggerEnableBoxChanged(QString featureName);  	// Cn:设置Frame Trigger Enable参数 | En:Set Frame Trigger Enable Parameter
	void OnFrameTriggerSourceBoxChanged(QString featureName); 	// Cn:设置Frame Trigger Source参数 | En:Set Frame Trigger Source Parameter
	void OnFrameTriggerRateEditPressed();					  	// Cn:设置Frame Internal Trigger Rate参数 | En:Set Frame Internal Trigger Rate Parameter
	void OnFrameTriggerSoftwareButtonClicked();				  	// Cn:执行Frame Trigger Software | En:Execute Frame Trigger Software Command
	void OnSGGetParamButtonClicked();							// Cn:获取采集卡参数 | En:Get SG Card Parameter
	void OnSGConfigSaveButtonClicked();							// Cn:保存采集卡配置参数文件 | En:Save Config File
	void OnSGConfigLoadButtonClicked();							// Cn:加载采集卡配置参数文件 | En:Load Config File

																/* Cn:参数获取与设置（相机） | En:Parameters Get and Set (Camera) */
	void OnExpoEditPressed();									// Cn:设置曝光时间 | En:Set Exposure Parameter
	void OnGainEditPressed();									// Cn:设置增益 | En:Set Gain Parameter
	void OnFrameRateEditPressed();								// Cn:设置帧率 | En:Set Frame Rate Parameter
	void OnPixelFormatBoxChanged(QString featureName);		   	// Cn:设置像素格式参数 | En:Set Pixel Format Parameter
	void OnTriggerModeBoxChanged(QString featureName);		   	// Cn:设置触发模式参数 | En:Set Trigger Mode Parameter
	void OnCamGetParamButtonClicked();							// Cn:获取相机参数 | En:Get Camera Parameters
	void OnCamConfigSaveButtonClicked();						// Cn:保存相机配置参数文件 | En:Save Config File
	void OnCamConfigLoadButtonClicked();						// Cn:加载相机配置参数文件 | En:Load Config File

																/* Cn:界面更新 | En:Interface Update */
	void deviceConnectionLost();								// Cn:处理相机失联事件 | En:Handle Camera Connection Loss Event
	void displayImage(const QImage& image);						// Cn:显示图像 | En:Display Image
	void updateStatisticsInfo(int imgCnt, int imgErrCnt,
		int imgLostCnt, int segmentsLostCnt, int segmentsResendCnt);// Cn:更新图像统计信息 | En:Update Image Statistics

private:
	/* Cn:初始化 | En:Initialization */
	void init();												// Cn:初始化软件 | En:Initializing Software
	void setupSignalConnections();								// Cn:设置信号连接 | En:Set Signal Connection

																/* Cn:工具函数 | En:Utility functions */
	void updateUI();											// Cn:更新界面控件状态 | En:Update Interface Control Status
	void showErrorMsg(const QString& csMessage, uint32_t errorCode);	// Cn:显示错误消息 | En:Show Error Message
	QString suffixToString(SAVE_IMAGE_TYPE enSaveType) const;	// Cn:生成图像文件尾缀 | En:Generate Image File Suffix
	int saveImageToFile(SAVE_IMAGE_TYPE enSaveType);			// Cn:保存图像到指定路径 | En:Save Image to Specified Path
	void setupEnumComboBox(VkCamera* camera, const char* feature,
		QComboBox* box, const QString& errorMsg);				// Cn:配置ComboBox控件子项 | En:Configuring ComboBox control sub-items

public:
	IMAGE_INFO			m_saveImgInfo;	                        // Cn:图像信息 | En:Image Data Length
	unsigned char*		m_saveImageBuf;	                        // Cn:图像数据缓存（用于临时存储待保存的原始图像数据） | En:Image Data Cache (used for temporarily storing raw image data to be saved)
	uint32_t			m_saveImageLen;	                        // Cn:图像数据长度 | En:Image Data Length
	bool				m_isSaving;		                        // Cn:图像保存状态标志 | En:Image Save Status Flag

private:
	VkCamera*			m_pSGCard;			                    // Cn:采集卡控制器实例（封装SDK底层操作） | En:SG Card Controller Example (Encapsulating SDK Underlying Operations)
	VkCamera*			m_pCamera;		                        // Cn:相机控制器实例 | En:Camera Controller Instance
	DeviceMap			m_devMap;			                    // Cn:设备映射表（存储枚举到的设备） | En:Device Mapping Table (Storage for enumerated devices)
	bool				m_ignoreChanges;                        // Cn:忽略UI变化标志（防止参数更新时的递归调用） | En:Ignore UI change flag (Prevents recursive calls during parameter updates)

private:
    Ui::BasicDemo_SG3000_CLClass ui;
};
