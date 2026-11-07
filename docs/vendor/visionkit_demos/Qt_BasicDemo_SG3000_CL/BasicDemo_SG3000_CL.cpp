#pragma execution_character_set("utf-8")
#include "BasicDemo_SG3000_CL.h"
#include <qmessagebox.h>
#include <qfiledialog.h>
#include <qdebug.h>

#define PIXEL_FORMAT_MONO10P	0x010A0046
#define PIXEL_FORMAT_MONO12P	0x010C0047

const QString tipText = "Received frames:%1, Error frames:%2, Lost frames:%3, Lost packets:%4, Resent packets:%5";

// Cn:图像回调函数 | En:Image callback function
void ImageCallback(IMAGE image, void* context)
{
	BasicDemo_SG3000_CL* ctx = reinterpret_cast<BasicDemo_SG3000_CL*> (context);
	if (image)
	{
		// Cn:获取图像信息 | En:Get image information
		IMAGE_INFO imgInfo;
		ctx->getDevController()->GetImageInfo(image, &imgInfo);

		// Cn:获取图像数据 | En:Get image data
		unsigned char* data = ctx->getDevController()->GetImageBuffer(image);
		// Cn:获取图像数据长度 | En:Get image data length
		int dataLen = ctx->getDevController()->GetBufferNeed(image);

		{
			// Cn:保存图像信息 | En:Save image information
			ctx->m_saveImgInfo = imgInfo;
			// Cn:保存原始图像数据（注意：m_saveImageBuf为多线程共享资源，通过m_isSaving实现简单互斥，非完全线程安全）
			// En:Save raw image data (Note: Shared multithreaded resource - m_isSaving provides basic mutex)
			if (!ctx->m_isSaving && ctx->m_saveImageBuf && dataLen > 0)
			{
				ctx->m_saveImageLen = dataLen;
				memcpy(ctx->m_saveImageBuf, data, ctx->m_saveImageLen);
			}
		}

		if (imgInfo.format == PIXEL_FORMAT_MONO8) {
			// Cn:拷贝数据并发送信号通知主线程 | En:Copy the data and send a signal to notify the main thread
			QImage oriImg = QImage(data, imgInfo.width, imgInfo.height, imgInfo.width, QImage::Format_Grayscale8).copy();
			emit ctx->imageReceived(oriImg);

		}
		else if (imgInfo.format == PIXEL_FORMAT_MONO10P || imgInfo.format == PIXEL_FORMAT_MONO12P) {
			// Cn:转换为8位图像用于显示 | En:Convert to 8-bit image for display
			IMAGE bit8Image = ctx->getDevController()->Unpack2Image8(image);
			unsigned char* bit8Data = ctx->getDevController()->GetImageBuffer(bit8Image);

			QImage oriImg = QImage(bit8Data, imgInfo.width, imgInfo.height, imgInfo.width, QImage::Format_Grayscale8).copy();
			emit ctx->imageReceived(oriImg);

			// Cn:释放转换后的图像资源 | En:Release the converted image resources
			ctx->getDevController()->ReleaseImageBuffer(bit8Image);
		}
		else {
			// Cn:其他像素格式的处理可以在此扩展 | En:Processing of other pixel formats can be extended here
		}

		// Cn:获取帧统计信息 | En:Get frame statistics
		STATISTICS rawStats = new Statistics_C();
		int ret = ctx->getDevController()->GetFrameStatistics(rawStats);
		if (ret == OK) {
			emit ctx->statisticsInfoUpdated(rawStats->imagesCount,
				rawStats->imagesErrCount,
				rawStats->imagesLostCount,
				rawStats->segmentsLostCount,
				rawStats->segmentsResendCount);
		}
		delete rawStats;

		// Cn:释放图像资源 | En:Release image resource
		ctx->getDevController()->ReleaseImageBuffer(image);
	}
}

// Cn:采集卡心跳超时回调函数 | En:SG Card heartbeat timeout callback function
void SGDisconnectCallback(unsigned int ev, void *context)
{
	// Cn:空实现-无需特殊处理 | En:Empty implementation - no special handling required
}

// Cn:相机心跳超时回调函数 | En:Camera heartbeat timeout callback function
void CamDisconnectCallback(unsigned int ev, void *context)
{
	auto ctx = reinterpret_cast<BasicDemo_SG3000_CL*>(context);
	// Cn:通知UI 设备已丢失 | En:Notify UI that the device is lost
	emit ctx->heartbeatTimeout();
}

BasicDemo_SG3000_CL::BasicDemo_SG3000_CL(QWidget *parent)
	: QMainWindow(parent)
{
	ui.setupUi(this);

	// Cn:初始化软件 | En:Initializing Software
	init();
	// Cn:设置信号连接 | En:Set signal connection
	setupSignalConnections();
}

BasicDemo_SG3000_CL::~BasicDemo_SG3000_CL()
{
	// Cn:释放采集卡资源 | En:Release SG Card resources
	if (m_pSGCard) {
		delete m_pSGCard;
		m_pSGCard = nullptr;
	}

	// Cn:释放相机资源 | En:Release camera resources
	if (m_pCamera) {
		delete m_pCamera;
		m_pCamera = nullptr;
	}

	// Cn:释放图像缓存 | En:Release image cache
	if (m_saveImageBuf) {
		delete[] m_saveImageBuf;
		m_saveImageBuf = nullptr;
	}

}

void BasicDemo_SG3000_CL::init()
{
	// Cn:初始化变量 | En:Initialize variables
	m_pCamera = nullptr;
	m_pSGCard = nullptr;
	m_isSaving = false;
	m_ignoreChanges = false;
	m_saveImageLen = 0;

	// Cn:图像缓存区分配（约512MB）| En:Image buffer allocation (approximately 512MB)
	m_saveImageBuf = new unsigned char[16384 * 16384 * 2];

	// Cn:初始化界面 | En:Initialization interface
	ui.sgOpenBtn->setEnabled(false);
	ui.sgCloseBtn->setEnabled(false);
	ui.camOpenBtn->setEnabled(false);
	ui.camCloseBtn->setEnabled(false);
	ui.sgTab->setEnabled(false);
	ui.camTab->setEnabled(false);
	ui.imgLabel->setStyleSheet("border: 1px solid black; background: transparent;");

	ui.sgStatusTip->setText("Disconnected");
	ui.camStatusTip->setText("Disconnected");
	ui.imgLabelTip->setText(tipText.arg(0).arg(0).arg(0).arg(0).arg(0));
}

void BasicDemo_SG3000_CL::setupSignalConnections()
{
	connect(ui.enumBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnEnumButtonClicked);
	connect(ui.sgListBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnSGListBoxChanged);
	connect(ui.sgOpenBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSGOpenButtonClicked);
	connect(ui.sgCloseBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSGCloseButtonClicked);
	connect(ui.camListBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnCamListBoxChanged);
	connect(ui.camOpenBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnCamOpenButtonClicked);
	connect(ui.camCloseBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnCamCloseButtonClicked);

	connect(ui.camLinkConfigBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnLinkConfigBoxChanged);
	connect(ui.camColorTypeBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnColorTypeBoxChanged);
	connect(ui.camPixelDepthBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnPixelDepthBoxChanged);
	connect(ui.camScanTypeBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnScanTypeBoxChanged);
	connect(ui.tapGeometryBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnTapGeometryBoxChanged);
	connect(ui.horizontalEdit, &QLineEdit::returnPressed, this, &BasicDemo_SG3000_CL::OnHorizontalEditPressed);
	connect(ui.verticalEdit, &QLineEdit::returnPressed, this, &BasicDemo_SG3000_CL::OnVerticalEditPressed);

	connect(ui.CC1Box, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnCC1BoxChanged);
	connect(ui.lineTriggerEnableBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnLineTriggerEnableBoxChanged);
	connect(ui.frameTriggerEnableBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnFrameTriggerEnableBoxChanged);
	connect(ui.lineTriggerSourceBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnLineTriggerSourceBoxChanged);
	connect(ui.frameTriggerSourceBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnFrameTriggerSourceBoxChanged);
	connect(ui.lineTriggerRateEdit, &QLineEdit::returnPressed, this, &BasicDemo_SG3000_CL::OnLineTriggerRateEditPressed);
	connect(ui.frameTriggerRateEdit, &QLineEdit::returnPressed, this, &BasicDemo_SG3000_CL::OnFrameTriggerRateEditPressed);
	connect(ui.lineTriggerSoftwareBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnLineTriggerSoftwareButtonClicked);
	connect(ui.frameTriggerSoftwareBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnFrameTriggerSoftwareButtonClicked);
	connect(ui.sgGetParamBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSGGetParamButtonClicked);
	connect(ui.sgConfigSaveBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSGConfigSaveButtonClicked);
	connect(ui.sgConfigLoadBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSGConfigLoadButtonClicked);

	connect(ui.expoEdit, &QLineEdit::returnPressed, this, &BasicDemo_SG3000_CL::OnExpoEditPressed);
	connect(ui.gainEdit, &QLineEdit::returnPressed, this, &BasicDemo_SG3000_CL::OnGainEditPressed);
	connect(ui.frameRateEdit, &QLineEdit::returnPressed, this, &BasicDemo_SG3000_CL::OnFrameRateEditPressed);
	connect(ui.pixelFormatBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnPixelFormatBoxChanged);
	connect(ui.triggerModeBox, &QComboBox::currentTextChanged, this, &BasicDemo_SG3000_CL::OnTriggerModeBoxChanged);
	connect(ui.camGetParamBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnCamGetParamButtonClicked);
	connect(ui.camConfigSaveBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnCamConfigSaveButtonClicked);
	connect(ui.camConfigLoadBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnCamConfigLoadButtonClicked);

	connect(ui.startGrabBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnStartGrabButtonClicked);
	connect(ui.stopGrabBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnStopGrabButtonClicked);
	connect(ui.saveRawBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSaveRawButtonClicked);
	connect(ui.saveBmpBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSaveBmpButtonClicked);
	connect(ui.saveJpegBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSaveJpegButtonClicked);
	connect(ui.saveTifBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSaveTifButtonClicked);
	connect(ui.savePngBtn, &QPushButton::clicked, this, &BasicDemo_SG3000_CL::OnSavePngButtonClicked);

	connect(this, &BasicDemo_SG3000_CL::heartbeatTimeout, this, &BasicDemo_SG3000_CL::deviceConnectionLost);
	connect(this, &BasicDemo_SG3000_CL::imageReceived, this, &BasicDemo_SG3000_CL::displayImage);
	connect(this, &BasicDemo_SG3000_CL::statisticsInfoUpdated, this, &BasicDemo_SG3000_CL::updateStatisticsInfo);
}

void BasicDemo_SG3000_CL::updateUI()
{
	// Cn:获取采集卡状态 | En:Get SG Card status
	bool sgConnected = false;
	bool sgGrabbing = false;
	if (m_pSGCard) {
		sgConnected = m_pSGCard->IsDeviceConnected() == OK;
		sgGrabbing = m_pSGCard->IsAcquisitionActive() == OK;
	}
	ui.sgListBox->setEnabled(!sgConnected);
	ui.sgOpenBtn->setEnabled(!sgConnected);
	ui.sgCloseBtn->setEnabled(sgConnected);
	ui.sgTab->setEnabled(sgConnected);
	ui.imgSaveGrp->setEnabled(sgConnected && sgGrabbing);

	QString sgStatusText = sgConnected ?
		(sgGrabbing ? "Acquiring Images" : "Connected") :
		"Disconnected";
	ui.sgStatusTip->setText(sgStatusText);

	// Cn:获取相机状态 | En:Get camera status
	bool camConnected = false;
	if (m_pCamera) {
		camConnected = m_pCamera->IsDeviceConnected() == OK;
	}

	ui.camListBox->setEnabled(!camConnected);
	ui.camOpenBtn->setEnabled(!camConnected);
	ui.camCloseBtn->setEnabled(camConnected);
	ui.camTab->setEnabled(camConnected);

	QString camStatusText = camConnected ? "Connected" : "Disconnected";
	ui.camStatusTip->setText(camStatusText);
}

void BasicDemo_SG3000_CL::OnEnumButtonClicked()
{
	ui.sgListBox->clear();
	ui.camListBox->clear();
	m_devMap.clear();

	// Cn:根据DeviceType枚举设备 | En:According to the DeviceType enumeration device
	m_devMap = EnumerateDevices(DeviceTypeSG | DeviceTypeGencp);
	if (m_devMap.empty()) {
		showErrorMsg("No devices found", -1);
		return;
	}

	for (const auto& pair : m_devMap) {
		const std::string& devName = pair.second.name;
		unsigned int devType = pair.second.type;
		//  Cn:根据DeviceType填充采集卡与相机列表 | En:Fill in the SG Card and camera list based on DeviceType
		if (devType == DeviceTypeSG) {
			ui.sgListBox->addItem(QString::fromStdString(devName));
		}
		else {
			ui.camListBox->addItem(QString::fromStdString(devName));
		}
	}
	updateUI();
}


/* Cn:采集卡相关函数 | En:SG Card-related functions */
void BasicDemo_SG3000_CL::OnSGOpenButtonClicked()
{
	if (m_pSGCard == nullptr) {
		showErrorMsg("Please select an SG card first", -1);
		return;
	}
	// Cn:连接设备 | En:Connect device
	int ret = m_pSGCard->Connect(SGDisconnectCallback, this);
	if (ret != OK) {
		updateUI();
		showErrorMsg("Failed to open SG card", ret);
		return;
	}

	// Cn:配置可变帧长功能 | En:Configure camera variable frame length function
	// @note Cn:使用线阵相机时需启用可变帧长功能
	//		 En:When using a line scan camera, the variable frame length function must be enabled.
	ret = m_pSGCard->SupportVariableFrame(1);
	if (ret != OK) {
		showErrorMsg("Failed to set variable frame length", ret);
		return;
	}

	// Cn:设置采集模式 | En:Set grab mode
	// @note Cn:GRAB_MODE_CALLBACK（自动通过回调函数传输图像）/GRAB_MODE_MANUAL（需主动调用VkGetOneFrame获取图像）
	//	     En:GRAB_MODE_CALLBACK(Images auto-delivered via callback)/GRAB_MODE_MANUAL(Actively call VkGetOneFrame to retrieve images)
	ret = m_pSGCard->SetAcquisitionMode(GRAB_MODE_CALLBACK);
	if (ret != OK) {
		showErrorMsg("Failed to set acquisition mode", ret);
		return;
	}

	// Cn:注册抓图回调 | En:Register image callback
	// @note Cn:GRAB_MODE_MANUAL模式也需在开始取流之前注册图像回调（但实际从VkGetOneFrame取图）
	//       En:GRAB_MODE_MANUAL mode also requires registering an image callback before starting acquisition (but actually retrieves images from VkGetOneFrame)
	ret = m_pSGCard->RegisterImageCallback(0, ImageCallback);
	if (ret != OK) {
		showErrorMsg("Failed to register image callback", ret);
		return;
	}

	// Cn:更新UI并获取参数 | En:Update UI and get parameters
	updateUI();
	OnSGGetParamButtonClicked();
}

void BasicDemo_SG3000_CL::OnSGCloseButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:断开设备 | En:Disconnect device
	int ret = m_pSGCard->Disconnect();
	if (ret != OK) {
		showErrorMsg("Failed to close SG card", ret);
	}
	updateUI();
}

void BasicDemo_SG3000_CL::OnSGListBoxChanged(QString sgCardName)
{
	CAMERA sgCardHandle = nullptr;
	// Cn:选择设备 | En:Select device by name
	for (const auto& pair : m_devMap) {
		const std::string& devName = pair.second.name;
		if (devName == sgCardName.toStdString()) {
			sgCardHandle = pair.first;
		}
	}

	if (sgCardHandle != nullptr) {
		// Cn:创建采集卡实例 | En:Create a SG card instance
		m_pSGCard = new VkCamera(sgCardHandle);
	}
}

void BasicDemo_SG3000_CL::OnStartGrabButtonClicked()
{
	if (!m_pSGCard) {
		showErrorMsg("Please select and connect an SG card first", -1);
		return;
	}

	// Cn:开启抓图 | En:Start Grabbing
	int ret = m_pSGCard->StartImageAcquisition(1000, 0, this);
	if (ret != OK) {
		showErrorMsg("Failed to start image acquisition", ret);
	}
	updateUI();
}

void BasicDemo_SG3000_CL::OnStopGrabButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:停止抓图 | En:Stop Grabbing
	int ret = m_pSGCard->StopImageAcquisition(0);
	if (ret != OK) {
		showErrorMsg("Failed to stop image acquisition", ret);
	}
	updateUI();
}

void BasicDemo_SG3000_CL::displayImage(const QImage& image)
{
	// Cn:缩放图像以适应QLable大小 | En:Scale the image to fit the QLabel size
	QImage scalImg = image.scaled(
		ui.imgLabel->size(),
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation
	);
	QPixmap pix = QPixmap::fromImage(scalImg);
	ui.imgLabel->setPixmap(pix);
}

void BasicDemo_SG3000_CL::updateStatisticsInfo(int imgCnt, int imgErrCnt, int imgLostCnt, int segmentsLostCnt, int segmentsResendCnt)
{
	// Cn:更新统计信息显示 | En:Update statistics show
	QString text = tipText.arg(imgCnt).arg(imgErrCnt).arg(imgLostCnt).arg(segmentsLostCnt).arg(segmentsResendCnt);
	ui.imgLabelTip->setText(text);

	update();
}

void BasicDemo_SG3000_CL::OnSaveRawButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = saveImageToFile(Image_Raw);
	if (ret != OK) {
		showErrorMsg("Failed to save Raw image", ret);
	}
}

void BasicDemo_SG3000_CL::OnSaveBmpButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = saveImageToFile(Image_Bmp);
	if (ret != OK) {
		showErrorMsg("Failed to save Bmp image", ret);
	}
}

void BasicDemo_SG3000_CL::OnSavePngButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = saveImageToFile(Image_Png);
	if (ret != OK) {
		showErrorMsg("Failed to save Png image", ret);
	}
}

void BasicDemo_SG3000_CL::OnSaveJpegButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = saveImageToFile(Image_Jpeg);
	if (ret != OK) {
		showErrorMsg("Failed to save Jpeg image", ret);
	}
}

void BasicDemo_SG3000_CL::OnSaveTifButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = saveImageToFile(Image_Tif);
	if (ret != OK) {
		showErrorMsg("Failed to save Tif image", ret);
	}
}

void BasicDemo_SG3000_CL::OnLinkConfigBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCard->SetEnumValue("ClConfiguration", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Camera Link Config", ret);
	}
}

void BasicDemo_SG3000_CL::OnColorTypeBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCard->SetEnumValue("CameraColorType", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Camera Color Type", ret);
	}
}

void BasicDemo_SG3000_CL::OnPixelDepthBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCard->SetEnumValue("CameraPixelDepth", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Camera Pixel Depth", ret);
	}
}

void BasicDemo_SG3000_CL::OnScanTypeBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCard->SetEnumValue("CameraScanType", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Camera Scan Type", ret);
	}
}

void BasicDemo_SG3000_CL::OnTapGeometryBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCard->SetEnumValue("DeviceTapGeometry", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Device Tap Geometry", ret);
	}
}

void BasicDemo_SG3000_CL::OnHorizontalEditPressed()
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Int型参数 | En:Set Int type parameters
	unsigned long long value = ui.horizontalEdit->text().toInt();
	int ret = m_pSGCard->SetIntValue("CameraWidth", value);
	if (ret != OK) {
		showErrorMsg("Failed to set Horizontal Active", ret);
	}
}

void BasicDemo_SG3000_CL::OnVerticalEditPressed()
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Int型参数 | En:Set Int type parameters
	unsigned long long value = ui.verticalEdit->text().toInt();
	int ret = m_pSGCard->SetIntValue("CameraHeight", value);
	if (ret != OK) {
		showErrorMsg("Failed to set Vertical Active", ret);
	}
}

void BasicDemo_SG3000_CL::OnCC1BoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCard->SetEnumValue("CL_CC1", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set CC1", ret);
	}
}

void BasicDemo_SG3000_CL::OnLineTriggerEnableBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Int型参数 | En:Set Int type parameters
	int ret = m_pSGCard->SetEnumValue("LineTriggerEnable", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Line Trigger Enable", ret);
	}
}

void BasicDemo_SG3000_CL::OnFrameTriggerEnableBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Int型参数 | En:Set Int type parameters
	int ret = m_pSGCard->SetEnumValue("FrameTriggerEnable", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Frame Trigger Enable", ret);
	}
}

void BasicDemo_SG3000_CL::OnLineTriggerSourceBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Int型参数 | En:Set Int type parameters
	int ret = m_pSGCard->SetEnumValue("LineTriggerSource", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Line Trigger Source", ret);
	}
}

void BasicDemo_SG3000_CL::OnLineTriggerRateEditPressed()
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Double型参数 | En:Set Double type parameters
	double value = ui.lineTriggerRateEdit->text().toDouble();
	int ret = m_pSGCard->SetDoubleValue("LineInternalTriggerRate", value);
	if (ret != OK) {
		showErrorMsg("Failed to set Internal Line Trigger Actual Freq (Hz)", ret);
	}
}

void BasicDemo_SG3000_CL::OnFrameTriggerSourceBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Int型参数 | En:Set Int type parameters
	int ret = m_pSGCard->SetEnumValue("FrameTriggerSource", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set Frame Trigger Source", ret);
	}
}

void BasicDemo_SG3000_CL::OnFrameTriggerRateEditPressed()
{
	if (m_ignoreChanges || !m_pSGCard) return;

	// Cn:设置Double型参数 | En:Set Double type parameters
	double value = ui.frameTriggerRateEdit->text().toDouble();
	int ret = m_pSGCard->SetDoubleValue("FrameInternalTriggerRate", value);
	if (ret != OK) {
		showErrorMsg("Failed to set Internal Frame Trigger Actual Freq (Hz)", ret);
	}
}

void BasicDemo_SG3000_CL::OnLineTriggerSoftwareButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:执行一次Command型命令 | En:Execute Command once
	int ret = m_pSGCard->ExecuteCommand("LineTriggerSoftware");
	if (ret != OK) {
		showErrorMsg("Failed to execute Line Trigger Software", ret);
	}
}

void BasicDemo_SG3000_CL::OnFrameTriggerSoftwareButtonClicked()
{
	if (!m_pSGCard) return;

	// Cn:执行一次Command型命令 | En:Execute Command once
	int ret = m_pSGCard->ExecuteCommand("FrameTriggerSoftware");
	if (ret != OK) {
		showErrorMsg("Failed to execute Frame Trigger Software", ret);
	}
}

void BasicDemo_SG3000_CL::OnSGConfigSaveButtonClicked()
{
	if (!m_pSGCard) return;

	QString fileName = QFileDialog::getSaveFileName(
		this,
		tr("Save SG Card Configuration File"),
		QDir::currentPath() + "/SGFeatureFile.cfg",
		tr("Configuration Files (*.cfg);;All Files (*.*)")
	);

	if (fileName.isEmpty()) {
		return;
	}

	// Cn:保存采集卡配置文件到本地 | En:Save camera configuration file to local
	int ret = m_pSGCard->ConfigSave(fileName.toStdString().c_str());
	if (ret != OK) {
		showErrorMsg("Failed to save SG card configuration file", ret);
	}
	else {
		QMessageBox::information(this, tr("Save Successful"), tr("SG card configuration has been successfully saved to: \n%1").arg(fileName));
	}
}

void BasicDemo_SG3000_CL::OnSGConfigLoadButtonClicked()
{
	if (!m_pSGCard) return;

	QString fileName = QFileDialog::getOpenFileName(
		this,
		tr("Load SG Card Configuration File"),
		QDir::currentPath(),
		tr("Configuration Files (*.cfg);;All Files (*.*)")
	);

	if (fileName.isEmpty()) {
		return;
	}

	// Cn:加载采集卡配置文件到设备 | En:Save camera configuration file to local
	int ret = m_pSGCard->ConfigLoad(fileName.toStdString().c_str());
	if (ret != OK) {
		showErrorMsg("Failed to load SG card configuration file", ret);
	}
	else {
		QMessageBox::information(this, tr("Load Successful"), tr("SG card configuration has been loaded from file: \n%1").arg(fileName));
	}
}

void BasicDemo_SG3000_CL::OnSGGetParamButtonClicked()
{
	if (!m_pSGCard) return;
	m_ignoreChanges = true;

	// Cn:设置所有枚举型参数 | En:Set all enumeration parameters
	setupEnumComboBox(m_pSGCard, "ClConfiguration", ui.camLinkConfigBox, "Failed to get Camera Link Config");
	setupEnumComboBox(m_pSGCard, "CameraColorType", ui.camColorTypeBox, "Failed to get Camera Color Type");
	setupEnumComboBox(m_pSGCard, "CameraPixelDepth", ui.camPixelDepthBox, "Failed to get Camera Pixel Depth");
	setupEnumComboBox(m_pSGCard, "CameraScanType", ui.camScanTypeBox, "Failed to get Camera Scan Type");
	setupEnumComboBox(m_pSGCard, "DeviceTapGeometry", ui.tapGeometryBox, "Failed to get Device Tap Geometry");
	setupEnumComboBox(m_pSGCard, "CL_CC1", ui.CC1Box, "Failed to get CC1");
	setupEnumComboBox(m_pSGCard, "LineTriggerSource", ui.lineTriggerSourceBox, "Failed to get Line Trigger Source");
	setupEnumComboBox(m_pSGCard, "FrameTriggerSource", ui.frameTriggerSourceBox, "Failed to get Frame Trigger Source");
	setupEnumComboBox(m_pSGCard, "LineTriggerEnable", ui.lineTriggerEnableBox, "Failed to get Line Trigger Enable");
	setupEnumComboBox(m_pSGCard, "FrameTriggerEnable", ui.frameTriggerEnableBox, "Failed to get Frame Trigger Enable");

	// Cn:获取Int型参数 | En:Get Int type parameters
	unsigned long long sgWidthData;
	int ret = m_pSGCard->GetIntValue("CameraWidth", &sgWidthData);
	if (ret != OK) {
		ui.horizontalEdit->clear();
		showErrorMsg("Failed to get Horizontal Active", ret);
	}
	else {
		ui.horizontalEdit->setText(QString::number(sgWidthData));
	}

	// Cn:获取Int型参数 | En:Get Int type parameters
	unsigned long long sgHeightData;
	ret = m_pSGCard->GetIntValue("CameraHeight", &sgHeightData);
	if (ret != OK) {
		ui.verticalEdit->clear();
		showErrorMsg("Failed to get Vertical Active", ret);
	}
	else {
		ui.verticalEdit->setText(QString::number(sgHeightData));
	}

	// Cn:获取Double型参数 | En:Get Double type parameters
	double sgLineRateData;
	ret = m_pSGCard->GetDoubleValue("LineInternalTriggerRate", &sgLineRateData);
	if (ret != OK) {
		ui.lineTriggerRateEdit->clear();
		showErrorMsg("Failed to get Internal Line Trigger Actual Freq (Hz)", ret);
	}
	else {
		ui.lineTriggerRateEdit->setText(QString::number(sgLineRateData, 'f', 2));
	}

	// Cn:获取Double型参数 | En:Get Double type parameters
	double sgFrameRateData;
	ret = m_pSGCard->GetDoubleValue("FrameInternalTriggerRate", &sgFrameRateData);
	if (ret != OK) {
		ui.frameTriggerRateEdit->clear();
		showErrorMsg("Failed to get Internal Frame Trigger Actual Freq (Hz)", ret);
	}
	else {
		ui.frameTriggerRateEdit->setText(QString::number(sgFrameRateData, 'f', 2));
	}
	m_ignoreChanges = false;
}


/* Cn:相机相关函数 | En:Camera-related functions */
void BasicDemo_SG3000_CL::OnCamOpenButtonClicked()
{
	if (m_pCamera == nullptr) {
		showErrorMsg("Please select a camera first", -1);
		return;
	}

	// Cn:连接设备 | En:Connect device
	int ret = m_pCamera->Connect(CamDisconnectCallback, this);
	if (ret != OK) {
		updateUI();
		showErrorMsg("Failed to open camera", ret);
		return;
	}

	// Cn:更新UI并获取参数 | En:Update UI and get parameters
	updateUI();
	OnCamGetParamButtonClicked();
}

void BasicDemo_SG3000_CL::OnCamCloseButtonClicked()
{
	if (!m_pCamera) return;

	// Cn:断开设备 | En:Disconnect device
	int ret = m_pCamera->Disconnect();
	if (ret != OK) {
		showErrorMsg("Failed to close camera", ret);
	}
	updateUI();
}

void BasicDemo_SG3000_CL::OnCamListBoxChanged(QString cameraName)
{
	CAMERA cameraHandle = nullptr;
	// Cn:选择设备 | En:Select device by name
	for (const auto& pair : m_devMap) {
		const std::string& devName = pair.second.name;
		if (devName == cameraName.toStdString()) {
			cameraHandle = pair.first;
		}
	}

	if (cameraHandle != nullptr) {
		// Cn:创建相机实例 | En:Create a Camera Instance
		m_pCamera = new VkCamera(cameraHandle);
	}
}

void BasicDemo_SG3000_CL::deviceConnectionLost()
{
	showErrorMsg("Camera heartbeat timeout", -1);

	// Cn:重置UI | En:Reset UI
	ui.camListBox->clear();

	ui.expoEdit->clear();
	ui.gainEdit->clear();
	ui.frameRateEdit->clear();
	ui.pixelFormatBox->clear();

	updateUI();
}

void BasicDemo_SG3000_CL::OnExpoEditPressed()
{
	if (!m_pCamera) return;

	// Cn:设置Int型参数 | En:Set Int type parameters
	int value = ui.expoEdit->text().toInt();
	int ret = m_pCamera->SetIntValue("ExposureTime", value);
	if (ret != OK) {
		showErrorMsg("Failed to set exposure time", ret);
	}
}

void BasicDemo_SG3000_CL::OnGainEditPressed()
{
	if (!m_pCamera) return;

	// Cn:设置Double型参数 | En:Set Double type parameters
	int value = ui.gainEdit->text().toInt();
	int ret = m_pCamera->SetIntValue("Gain", value);
	if (ret != OK) {
		showErrorMsg("Failed to set image gain", ret);
	}
}

void BasicDemo_SG3000_CL::OnFrameRateEditPressed()
{
	if (!m_pCamera) return;

	// Cn:设置Int型参数 | En:Set Int type parameters
	int value = ui.frameRateEdit->text().toInt();
	int ret = m_pCamera->SetIntValue("FrameRate", value);
	if (ret != OK) {
		showErrorMsg("Failed to set frame rate", ret);
	}
}

void BasicDemo_SG3000_CL::OnPixelFormatBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pCamera) return;

	int ret = m_pCamera->SetEnumValue("PixelFormat", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set pixel format", ret);
	}
}

void BasicDemo_SG3000_CL::OnTriggerModeBoxChanged(QString featureName)
{
	if (m_ignoreChanges || !m_pCamera) return;

	int ret = m_pCamera->SetEnumValue("TriggerMode", featureName.toUtf8().constData());
	if (ret != OK) {
		showErrorMsg("Failed to set trigger mode", ret);
	}
}

void BasicDemo_SG3000_CL::OnCamGetParamButtonClicked()
{
	if (!m_pCamera) return;
	m_ignoreChanges = true;

	// Cn:设置相机枚举参数 | En:Set camera enumeration parameters
	setupEnumComboBox(m_pCamera, "PixelFormat", ui.pixelFormatBox, "Failed to get pixel format");
	setupEnumComboBox(m_pCamera, "TriggerMode", ui.triggerModeBox, "Failed to get trigger mode");

	// Cn:获取Int型参数 | En:Get Int type parameters
	unsigned long long m_expo_data;
	int ret = m_pCamera->GetIntValue("ExposureTime", &m_expo_data);
	if (ret != OK) {
		showErrorMsg("Failed to get exposure time (Exposure)", ret);
	}
	else {
		ui.expoEdit->setText(QString::number(m_expo_data));
	}

	// Cn:获取Double型参数 | En:Get Double type parameters
	unsigned long long m_gain_data;
	ret = m_pCamera->GetIntValue("Gain", &m_gain_data);
	if (ret != OK) {
		showErrorMsg("Failed to get image gain (Gain)", ret);
	}
	else {
		ui.gainEdit->setText(QString::number(m_gain_data));
	}

	// Cn:获取Int型参数 | En:Get Int type parameters
	unsigned long long m_framerate_data;
	ret = m_pCamera->GetIntValue("FrameRate", &m_framerate_data);
	if (ret != OK) {
		showErrorMsg("Failed to get frame rate (FrameRate)", ret);
	}
	else {
		ui.frameRateEdit->setText(QString::number(m_framerate_data));
	}
	m_ignoreChanges = false;
}

void BasicDemo_SG3000_CL::OnCamConfigSaveButtonClicked()
{
	if (!m_pCamera) return;

	QString fileName = QFileDialog::getSaveFileName(
		this,
		tr("Save Camera Configuration File"),
		QDir::currentPath() + "/FeatureFile.cfg",
		tr("Configuration Files (*.cfg);;All Files (*.*)")
	);

	if (fileName.isEmpty()) {
		return;
	}

	// Cn:保存相机配置文件到本地 | En:Save camera configuration file to local
	int ret = m_pCamera->ConfigSave(fileName.toStdString().c_str());
	if (ret != OK) {
		showErrorMsg("Failed to save camera configuration file", ret);
	}
	else {
		QMessageBox::information(this, tr("Save Successful"), tr("Camera configuration has been successfully saved to: \n%1").arg(fileName));
	}
}

void BasicDemo_SG3000_CL::OnCamConfigLoadButtonClicked()
{
	if (!m_pCamera) return;

	QString fileName = QFileDialog::getOpenFileName(
		this,
		tr("Load Camera Configuration File"),
		QDir::currentPath(),
		tr("Configuration Files (*.cfg);;All Files (*.*)")
	);

	if (fileName.isEmpty()) {
		return;
	}

	// Cn:加载相机配置文件到设备 | En:Load camera configuration file to device
	int ret = m_pCamera->ConfigLoad(fileName.toStdString().c_str());
	if (ret != OK) {
		showErrorMsg("Failed to load camera configuration file", ret);
	}
	else {
		QMessageBox::information(this, tr("Load Successful"), tr("Camera configuration has been loaded from file: \n%1").arg(fileName));
	}
}


/* Cn:工具函数 | En:Utility functions */
void BasicDemo_SG3000_CL::showErrorMsg(const QString& csMessage, uint32_t errorCode)
{
	if (errorCode != OK) {
		QString errorMsg;
		errorMsg = QString("%1: Error = 0x%2").arg(csMessage).arg(errorCode, 0, 16);

		if (errorCode == 0xFA000006) errorMsg += "\n(Node type mismatch, please check the node type)";
		if (errorCode == 0xFA000008) errorMsg += "\n(Node name not found, please check the node name)";

		QMessageBox::warning(this, "Error", errorMsg);
	}
}

QString BasicDemo_SG3000_CL::suffixToString(SAVE_IMAGE_TYPE type) const
{
	static const QMap<SAVE_IMAGE_TYPE, QString> postfixMap = {
		{ Image_Raw,  ".raw" },
		{ Image_Bmp,  ".bmp" },
		{ Image_Png,  ".png" },
		{ Image_Jpeg, ".jpg" },
		{ Image_Tif,  ".tif" }
	};
	return postfixMap.value(type, ".dat");
}

int BasicDemo_SG3000_CL::saveImageToFile(SAVE_IMAGE_TYPE enSaveImageType)
{
	if (m_saveImageLen == 0 || m_saveImageBuf == nullptr) {
		showErrorMsg("No image data available", -1);
		return -1;
	}

	// Cn:设置保存参数 | En:Save Settings Parameters
	IMAGE_META stImageInfo;
	memset(&stImageInfo, 0, sizeof(IMAGE_META));
	SAVE_IMAGE_PARAM stImageParam;
	memset(&stImageParam, 0, sizeof(SAVE_IMAGE_PARAM));

	m_isSaving = true;
	stImageInfo.nWidth = m_saveImgInfo.width;
	stImageInfo.nHeight = m_saveImgInfo.height;
	stImageInfo.enPixelFormat = static_cast<IMAGE_META_FORMAT>(m_saveImgInfo.format);
	stImageInfo.pImageBuffer = m_saveImageBuf;
	stImageInfo.nImageBufferLength = m_saveImageLen;

	// Cn:jpg图像质量范围为(50-99) | En:jpg image nQuality range is (50-99)
	stImageParam.enImageType = enSaveImageType;
	stImageParam.iMethodValue = 1;
	stImageParam.nQuality = 98;

	char chImagePath[256] = { 0 };
	if (Image_Raw == stImageParam.enImageType)
	{
		sprintf_s(chImagePath, 256, "Image_w%d_h%d.raw", m_saveImgInfo.width, m_saveImgInfo.height);
	}
	else if (Image_Bmp == stImageParam.enImageType)
	{
		sprintf_s(chImagePath, 256, "Image_w%d_h%d.bmp", m_saveImgInfo.width, m_saveImgInfo.height);
	}
	else if (Image_Jpeg == stImageParam.enImageType)
	{
		sprintf_s(chImagePath, 256, "Image_w%d_h%d.jpg", m_saveImgInfo.width, m_saveImgInfo.height);
	}
	else if (Image_Tif == stImageParam.enImageType)
	{
		sprintf_s(chImagePath, 256, "Image_w%d_h%d.tif", m_saveImgInfo.width, m_saveImgInfo.height);
	}
	else if (Image_Png == stImageParam.enImageType)
	{
		sprintf_s(chImagePath, 256, "Image_w%d_h%d.png", m_saveImgInfo.width, m_saveImgInfo.height);
	}

	QString savePath = QFileDialog::getSaveFileName(
		this,
		tr("Save Image"),
		QDir::current().filePath(chImagePath),
		tr("%1 Files (*%2);;All Files (*.*)")
		.arg(suffixToString(stImageParam.enImageType).mid(1).toUpper())
		.arg(suffixToString(stImageParam.enImageType))
	);

	int ret = OK;
	if (!savePath.isEmpty()) {
		// Cn:保存相机图像到文件 | En:Save image to file
		ret = m_pSGCard->SaveImage(&stImageInfo, &stImageParam, savePath.toStdString().c_str());
	}
	m_isSaving = false;
	return ret;
}

void BasicDemo_SG3000_CL::setupEnumComboBox(VkCamera* dev, const char* feature, QComboBox* box, const QString& errorMsg)
{
	// Cn:获取枚举项数量 | En:Get the number of enumeration items
	int enumCnt = 0;
	int ret = dev->GetFeatureEnumArray(feature, nullptr, &enumCnt);
	if (ret != OK || enumCnt < 0) {
		showErrorMsg(errorMsg, ret);
		return;
	}

	// Cn:获取枚举项 | En:Get enumeration items
	std::vector<ENUMARRAY> enumData(enumCnt);
	ret = dev->GetFeatureEnumArray(feature, enumData.data(), &enumCnt);
	if (ret != OK) {
		showErrorMsg(errorMsg, ret);
		return;
	}

	box->clear();
	for (const auto& item : enumData) {
		box->addItem(QString::fromUtf8(item.entry));
	}

	// Cn:获取当前值并设置选中项 | En:Get the current value and set the selected item
	char value[256];
	ret = dev->GetEnumValue(feature, value);
	if (ret != OK) {
		showErrorMsg(errorMsg, ret);
	}
	else {
		box->setCurrentText(QString::fromUtf8(value));
	}
}
