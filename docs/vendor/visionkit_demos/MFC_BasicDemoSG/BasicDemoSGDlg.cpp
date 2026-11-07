
#include "stdafx.h"
#include "BasicDemoSG.h"
#include "BasicDemoSGDlg.h"
#include "afxdialogex.h"
#include <memory>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define PIXEL_FORMAT_MONO10P	0x010A0046
#define PIXEL_FORMAT_MONO12P	0x010C0047

// Cn:图像回调函数 | En:Image callback function
void ImageCallback(IMAGE image, void* context)
{
	CBasicDemoSGDlg* ctx = reinterpret_cast<CBasicDemoSGDlg*> (context);
	if (image)
	{
		// Cn:获取图像信息 | En:Get image information
		IMAGE_INFO imgInfo;
		ctx->GetDevController()->GetImageInfo(image, &imgInfo);

		// Cn:获取图像数据 | En:Get image data
		unsigned char* data = ctx->GetDevController()->GetImageBuffer(image);
		// Cn:获取图像数据长度 | En:Get image data length
		int dataLen = ctx->GetDevController()->GetBufferNeed(image);

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
			unsigned char* displayData = new unsigned char[dataLen];
			memcpy(displayData, data, dataLen);

			ImageDisplayData* pDisplayData = new ImageDisplayData();
			pDisplayData->pImgData = displayData;
			pDisplayData->stImgInfo = imgInfo;
			::PostMessage(ctx->GetSafeHwnd(), WM_UPDATE_IMAGE, (WPARAM)pDisplayData, 0);

		}
		else if (imgInfo.format == PIXEL_FORMAT_MONO10P || imgInfo.format == PIXEL_FORMAT_MONO12P) {
			// Cn:转换为8位图像用于显示 | En:Convert to 8-bit image for display
			IMAGE bit8Image = ctx->GetDevController()->Unpack2Image8(image);
			unsigned char* bit8Data = ctx->GetDevController()->GetImageBuffer(bit8Image);
			int bit8DataLen = ctx->GetDevController()->GetBufferNeed(bit8Image);

			unsigned char* displayData = new unsigned char[bit8DataLen];
			memcpy(displayData, bit8Data, bit8DataLen);

			ImageDisplayData* pDisplayData = new ImageDisplayData();
			pDisplayData->pImgData = displayData;
			pDisplayData->stImgInfo = imgInfo;
			::PostMessage(ctx->GetSafeHwnd(), WM_UPDATE_IMAGE, (WPARAM)pDisplayData, 0);

			// Cn:释放转换后的图像资源 | En:Release the converted image resources
			ctx->GetDevController()->ReleaseImageBuffer(bit8Image);
		}
		else {
			// Cn:其他像素格式的处理可以在此扩展 | En:Processing of other pixel formats can be extended here
		}

		// Cn:获取帧统计信息 | En:Get frame statistics
		STATISTICS rawStats = new Statistics_C();
		int ret = ctx->GetDevController()->GetFrameStatistics(rawStats);
		if (ret == OK) {
			Statistics* pStats = new Statistics();
			pStats->nImgCnt = rawStats->imagesCount;
			pStats->nImgErrCnt = rawStats->imagesErrCount;
			pStats->nImgLostCnt = rawStats->imagesLostCount;
			pStats->nSegmentsLostCnt = rawStats->segmentsLostCount;
			pStats->nSegmentsResendCnt = rawStats->segmentsResendCount;
			::PostMessage(ctx->GetSafeHwnd(), WM_UPDATE_STATISTICS, (WPARAM)pStats, 0);
		}
		delete rawStats;

		// Cn:释放图像资源 | En:Release image resource
		ctx->GetDevController()->ReleaseImageBuffer(image);
	}
}

// Cn:采集卡心跳超时回调函数 | En:SG card heartbeat timeout callback function
void SGDisconnectCallback(unsigned int ev, void *context)
{
	// Cn:空实现-无需特殊处理 | En:Empty implementation - no special handling required
}

// Cn:相机心跳超时回调函数 | En:Camera heartbeat timeout callback function
void CamDisconnectCallback(unsigned int ev, void *context)
{
	auto ctx = reinterpret_cast<CBasicDemoSGDlg*>(context);
	// Cn:通知UI 设备已丢失 | En:Notify UI that the device is lost
	if (ctx) {
		::PostMessage(ctx->GetSafeHwnd(), WM_HEARTBEAT_TIMEOUT, 0, 0);
	}
}

CBasicDemoSGDlg::CBasicDemoSGDlg(CWnd* pParent /*=NULL*/)
	: CDialogEx(IDD_BASICDEMOSG_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	ZeroMemory(&m_saveImgInfo, sizeof(IMAGE_INFO));
	ZeroMemory(&m_stats, sizeof(Statistics));
}

CBasicDemoSGDlg::~CBasicDemoSGDlg() {
	// Cn:释放采集卡资源 | En:Release SG card resources
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

void CBasicDemoSGDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_SG_LIST_COMBO, m_cbSGCardList);
	DDX_Control(pDX, IDC_CAM_LIST_COMBO, m_cbCameraList);
	DDX_Control(pDX, IDC_DEV_TAB, m_tabCtrl);
}

BEGIN_MESSAGE_MAP(CBasicDemoSGDlg, CDialogEx)
	ON_NOTIFY(TCN_SELCHANGE, IDC_DEV_TAB, &CBasicDemoSGDlg::OnTcnSelchangeDevTab)
	ON_BN_CLICKED(IDC_ENUM_BUTTON, &CBasicDemoSGDlg::OnBnClickedEnumButton)
	ON_BN_CLICKED(IDC_SG_OPEN_BUTTON, &CBasicDemoSGDlg::OnBnClickedSgOpenButton)
	ON_BN_CLICKED(IDC_SG_CLOSE_BUTTON, &CBasicDemoSGDlg::OnBnClickedSgCloseButton)
	ON_CBN_SELCHANGE(IDC_SG_LIST_COMBO, &CBasicDemoSGDlg::OnCbnSelchangeSgListCombo)
	ON_BN_CLICKED(IDC_CAM_OPEN_BUTTON, &CBasicDemoSGDlg::OnBnClickedCamOpenButton)
	ON_BN_CLICKED(IDC_CAM_CLOSE_BUTTON, &CBasicDemoSGDlg::OnBnClickedCamCloseButton)
	ON_CBN_SELCHANGE(IDC_CAM_LIST_COMBO, &CBasicDemoSGDlg::OnCbnSelchangeCamListCombo)

	ON_MESSAGE(WM_UPDATE_IMAGE, &CBasicDemoSGDlg::OnDisplayImage)
	ON_MESSAGE(WM_UPDATE_STATISTICS, &CBasicDemoSGDlg::OnUpdateStats)
	ON_MESSAGE(WM_HEARTBEAT_TIMEOUT, &CBasicDemoSGDlg::OnDevConnectLost)
	ON_MESSAGE(WM_UPDATE_UI, &CBasicDemoSGDlg::OnUpdateUI)
	ON_MESSAGE(WM_SHOW_ERROR_MSG, &CBasicDemoSGDlg::OnShowErrorMsg)
	ON_MESSAGE(WM_SG_SAVE_RAW, &CBasicDemoSGDlg::OnSaveRaw)
	ON_MESSAGE(WM_SG_SAVE_BMP, &CBasicDemoSGDlg::OnSaveBmp)
	ON_MESSAGE(WM_SG_SAVE_JPEG, &CBasicDemoSGDlg::OnSaveJpeg)
	ON_MESSAGE(WM_SG_SAVE_TIF, &CBasicDemoSGDlg::OnSaveTif)
	ON_MESSAGE(WM_SG_SAVE_PNG, &CBasicDemoSGDlg::OnSavePng)
END_MESSAGE_MAP()

BOOL CBasicDemoSGDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();
	SetIcon(m_hIcon, TRUE);	
	SetIcon(m_hIcon, FALSE);

	// Cn:初始化软件 | En:Initializing Software
	Init();

	return TRUE;  
}

HCURSOR CBasicDemoSGDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CBasicDemoSGDlg::SetupTab() {
    m_tabCtrl.InsertItem(0, _T("Grab Card"));
    m_tabCtrl.InsertItem(1, _T("Camera"));
	m_tabSGCard.Create(IDD_SG_TAB, &m_tabCtrl);
	m_tabCamera.Create(IDD_CAMERA_TAB, &m_tabCtrl);
	m_tabSGCard.SetParentDlg(this);
	m_tabCamera.SetParentDlg(this);

	CRect rect;
	m_tabCtrl.GetClientRect(&rect);
	rect.top += 20;
	rect.DeflateRect(1, 1, 1, 1);

	m_tabSGCard.MoveWindow(&rect);
	m_tabCamera.MoveWindow(&rect);

	m_tabSGCard.ShowWindow(SW_SHOW);
	m_tabCamera.ShowWindow(SW_HIDE);
	m_tabCtrl.SetCurSel(0);
}

void CBasicDemoSGDlg::Init() {
	// Cn:初始化变量 | En:Initialize variables
	m_wndDisplay = nullptr;
	m_pCamera = nullptr;
	m_pSGCard = nullptr;
	m_isSaving = false;
	m_saveImageLen = 0;

	// Cn:初始化并配置标签页控件 | En:Initialize and configure tab control
	SetupTab();

	// Cn:图像缓存区分配（约512MB）| En:Image buffer allocation (approximately 512MB)
	m_saveImageBuf = new unsigned char[16384 * 16384 * 2];

	// Cn:界面初始化 | En:Interface Initialization
	m_wndPictureCtrl.SubclassDlgItem(IDC_IMG_STATIC, this);
	m_wndPictureCtrl.ModifyStyle(0, SS_BLACKFRAME);

	m_wndStatsCtrl.SubclassDlgItem(IDC_IMG_TIP_STATIC, this);
	m_wndStatsCtrl.SetFont(GetFont());

	m_wndDisplay = GetDlgItem(IDC_IMG_STATIC);
	
	// Cn:设置初始控件状态信息 | En:Set initial control state
	Statistics* pStats = new Statistics();
	::PostMessage(this->GetSafeHwnd(), WM_UPDATE_STATISTICS, (WPARAM)pStats, 0);
	OnUpdateUI(0, 0);

	GetDlgItem(IDC_SG_OPEN_BUTTON)->EnableWindow(FALSE);
	GetDlgItem(IDC_CAM_OPEN_BUTTON)->EnableWindow(FALSE);
}

LRESULT CBasicDemoSGDlg::OnDisplayImage(WPARAM wParam, LPARAM lParam)
{
	ImageDisplayData* pImg = reinterpret_cast<ImageDisplayData*>(wParam);
	if (pImg) {
		DisplayImage(pImg->pImgData, pImg->stImgInfo);
		delete[] pImg->pImgData;
		delete pImg;
	}
	return 0;
}

LRESULT CBasicDemoSGDlg::OnUpdateStats(WPARAM wParam, LPARAM)
{
	std::unique_ptr<Statistics> stats(reinterpret_cast<Statistics*>(wParam));

	{
		CSingleLock lock(&m_csStats, TRUE);
		m_stats = *stats;
	}

	CString strText;
    strText.Format(_T("Received Frames: %d, Error Frames: %d, Lost Frames: %d, Lost Packets: %d, Resent Packets: %d"),
		m_stats.nImgCnt, m_stats.nImgErrCnt, m_stats.nImgLostCnt, m_stats.nSegmentsLostCnt, m_stats.nSegmentsResendCnt);

	m_wndStatsCtrl.SetWindowText(strText);

	return 0;
}

LRESULT CBasicDemoSGDlg::OnDevConnectLost(WPARAM wParam, LPARAM lParam)
{
    CString* pMsg = new CString(_T("Camera heartbeat timeout"));
	::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));

	// Cn:重置UI | En:Reset UI
	m_cbCameraList.ResetContent();

	::PostMessage(this->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
	return 0;
}

LRESULT CBasicDemoSGDlg::OnUpdateUI(WPARAM wParam, LPARAM lParam){
	// Cn:获取采集卡状态并更新相关控件状态 | En:Get SG card status and update the relevant control status.
	bool sgConnected = false;
	bool sgGrabbing = false;
	if (m_pSGCard) {
		sgConnected = m_pSGCard->IsDeviceConnected() == OK;
		sgGrabbing = m_pSGCard->IsAcquisitionActive() == OK;
	}

	GetDlgItem(IDC_SG_LIST_COMBO)->EnableWindow(!sgConnected);
	GetDlgItem(IDC_SG_OPEN_BUTTON)->EnableWindow(!sgConnected);
	GetDlgItem(IDC_SG_CLOSE_BUTTON)->EnableWindow(sgConnected);
	m_tabSGCard.EnableWindow(sgConnected);

	CString sgStatusText = sgConnected ?
		(sgGrabbing ? _T("Acquiring Images") : _T("Connected")) :
		_T("Disconnected");

	GetDlgItem(IDC_SG_STATUS_STATIC)->SetWindowTextW(sgStatusText);

	// Cn:获取相机状态并更新相关控件状态 | En:Get camera status and update related control status
	bool camConnected = false;
	if (m_pCamera) {
		camConnected = m_pCamera->IsDeviceConnected() == OK;
	}

	GetDlgItem(IDC_CAM_LIST_COMBO)->EnableWindow(!camConnected);
	GetDlgItem(IDC_CAM_OPEN_BUTTON)->EnableWindow(!camConnected);
	GetDlgItem(IDC_CAM_CLOSE_BUTTON)->EnableWindow(camConnected);
	m_tabCamera.EnableWindow(camConnected);

	CString camStatusText = camConnected ? _T("Connected") : _T("Disconnected");
	SetDlgItemText(IDC_CAM_STATUS_STATIC, camStatusText);
	return 0;
}

LRESULT CBasicDemoSGDlg::OnShowErrorMsg(WPARAM wParam, LPARAM lParam) {
	CString* pMessage = reinterpret_cast<CString*>(wParam);
	int errorCode = static_cast<int>(lParam);

	CString strErrorMsg;
	if (errorCode != OK) {
		strErrorMsg.Format(_T("%s:Error = 0x%08X"), pMessage ? *pMessage : _T("Unknown error"), errorCode);
	}
	else {
		strErrorMsg = pMessage ? *pMessage : _T("Unknown error");
	}
	AfxMessageBox(strErrorMsg, MB_ICONWARNING | MB_OK);
	if (pMessage) {
		delete pMessage;
	}
	return 0;
}

LRESULT CBasicDemoSGDlg::OnSaveRaw(WPARAM wParam, LPARAM lParam) {
	if (!m_pSGCard) return 0;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = SaveImageToFile(Image_Raw);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to save Raw image"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	return 0;
}

LRESULT CBasicDemoSGDlg::OnSaveBmp(WPARAM wParam, LPARAM lParam) {
	if (!m_pSGCard) return 0;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = SaveImageToFile(Image_Bmp);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to save Bmp image"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	return 0;
}

LRESULT CBasicDemoSGDlg::OnSaveJpeg(WPARAM wParam, LPARAM lParam) {
	if (!m_pSGCard) return 0;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = SaveImageToFile(Image_Jpeg);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to save Jpeg image"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	return 0;
}

LRESULT CBasicDemoSGDlg::OnSaveTif(WPARAM wParam, LPARAM lParam) {
	if (!m_pSGCard) return 0;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = SaveImageToFile(Image_Tif);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to save Tif image"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	return 0;
}

LRESULT CBasicDemoSGDlg::OnSavePng(WPARAM wParam, LPARAM lParam) {
	if (!m_pSGCard) return 0;

	// Cn:保存图像到指定路径 | En:Save image to specified path
	int ret = SaveImageToFile(Image_Png);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to save Png image"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	return 0;
}


void CBasicDemoSGDlg::OnTcnSelchangeDevTab(NMHDR *pNMHDR, LRESULT *pResult)
{
	int sel = m_tabCtrl.GetCurSel();
	m_tabSGCard.ShowWindow(sel == 0 ? SW_SHOW : SW_HIDE);
	m_tabCamera.ShowWindow(sel == 1 ? SW_SHOW : SW_HIDE);
	*pResult = 0;
}

void CBasicDemoSGDlg::OnBnClickedEnumButton()
{
	m_cbSGCardList.ResetContent();
	m_cbCameraList.ResetContent();
	m_devMap.clear();

	// Cn:根据DeviceType枚举设备 | En:According to the DeviceType enumeration device
	m_devMap = EnumerateDevices(DeviceTypeSG | DeviceTypeGencp);
    if (m_devMap.empty())
    {
        CString* pMsg = new CString(_T("No available devices found"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		return;
	}

	for (const auto& pair : m_devMap) {
		CString strDeviceName(pair.second.name.c_str());
		unsigned int devType = pair.second.type;
		//  Cn:根据DeviceType填充采集卡与相机列表 | En:Fill in the SG card and camera list based on DeviceType
		if (devType == DeviceTypeSG) {
			m_cbSGCardList.AddString(strDeviceName);
		}
		else {
			m_cbCameraList.AddString(strDeviceName);
		}
	}

	if (m_cbSGCardList.GetCount() > 0)
	{
		m_cbSGCardList.SetCurSel(0);
		PostMessage(WM_COMMAND, MAKEWPARAM(IDC_SG_LIST_COMBO, CBN_SELCHANGE), (LPARAM)m_cbSGCardList.m_hWnd);
	}

	if (m_cbCameraList.GetCount() > 0)
	{
		m_cbCameraList.SetCurSel(0);
		PostMessage(WM_COMMAND, MAKEWPARAM(IDC_CAM_LIST_COMBO, CBN_SELCHANGE), (LPARAM)m_cbCameraList.m_hWnd);
	}

	::PostMessage(this->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
}

void CBasicDemoSGDlg::OnBnClickedSgOpenButton()
{
    if (m_pSGCard == nullptr)
    {
        CString* pMsg = new CString(_T("Please select an SG card first"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		return;
	}
	// Cn:连接设备 | En:Connect device
	int ret = m_pSGCard->Connect(SGDisconnectCallback, this);
	if (ret != OK) {
		::PostMessage(this->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
        CString* pMsg = new CString(_T("Failed to open SG card"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
		return;
	}

	// Cn:配置可变帧长功能 | En:Configure camera variable frame length function
	// @note Cn:使用线阵相机时需启用可变帧长功能
	//		 En:When using a line scan camera, the variable frame length function must be enabled.
	ret = m_pSGCard->SupportVariableFrame(1);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set variable frame length"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
		return;
	}

	// Cn:设置采集模式 | En:Set grab mode
	// @note Cn:GRAB_MODE_CALLBACK（自动通过回调函数传输图像）/GRAB_MODE_MANUAL（需主动调用VkGetOneFrame获取图像）
	//	     En:GRAB_MODE_CALLBACK(Images auto-delivered via callback)/GRAB_MODE_MANUAL(Actively call VkGetOneFrame to retrieve images)
	ret = m_pSGCard->SetAcquisitionMode(GRAB_MODE_CALLBACK);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set grab mode"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
		return;
	}

	// Cn:注册抓图回调 | En:Register image callback
	// @note Cn:GRAB_MODE_MANUAL模式也需在开始取流之前注册图像回调（但实际从VkGetOneFrame取图）
	//       En:GRAB_MODE_MANUAL mode also requires registering an image callback before starting 
	//          acquisition (but actually retrieves images from VkGetOneFrame)
	ret = m_pSGCard->RegisterImageCallback(0, ImageCallback);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to register image callback"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
		return;
	}

	// Cn:将采集卡实例传递给子页面 | En:Pass SG card controller instance to subpage
	// @note 使子页面能直接调用采集卡控制接口
	m_tabSGCard.SetDeviceController(m_pSGCard);

	// Cn:更新UI | En:Update UI
	::PostMessage(this->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
}

void CBasicDemoSGDlg::OnBnClickedSgCloseButton()
{
	if (!m_pSGCard) return;

	// Cn:断开设备 | En:Disconnect device
	int ret = m_pSGCard->Disconnect();
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to close SG card"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	::PostMessage(this->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
}

void CBasicDemoSGDlg::OnCbnSelchangeSgListCombo()
{
	int nSel = m_cbSGCardList.GetCurSel();
	if (nSel == CB_ERR)return;

	CString strSelected;
	m_cbSGCardList.GetLBText(nSel, strSelected);

	CT2CA utf8Name(strSelected, CP_UTF8);
	std::string strSgCardName(utf8Name);

	CAMERA sgCardHandle = nullptr;
	// Cn:选择设备 | En:Select device by name
	for (const auto& pair : m_devMap) {
		const std::string& devName = pair.second.name;
		if (devName == strSgCardName) {
			sgCardHandle = pair.first;
		}
	}

	if (sgCardHandle != nullptr) {
		// Cn:创建采集卡实例 | En:Create a SG card instance
		m_pSGCard = new VkCamera(sgCardHandle);
	}
}

void CBasicDemoSGDlg::OnBnClickedCamOpenButton()
{
    if (m_pCamera == nullptr)
    {
        CString* pMsg = new CString(_T("Please select a camera first"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		return;
	}
	// Cn:连接设备 | En:Connect device
	int ret = m_pCamera->Connect(CamDisconnectCallback, this);
	if (ret != OK) {
		::PostMessage(this->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);

        CString* pMsg = new CString(_T("Failed to open camera"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
		return;
	}

	// Cn:将相机实例传递给子页面 | En:Pass camera controller instance to subpage
	// @note 使子页面能直接调用相机控制接口
	m_tabCamera.SetDeviceController(m_pCamera);

	// Cn:更新UI | En:Update UI
	::PostMessage(this->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
}

void CBasicDemoSGDlg::OnBnClickedCamCloseButton()
{
	if (!m_pCamera) return;

	// Cn:断开设备 | En:Disconnect device
	int ret = m_pCamera->Disconnect();
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to close camera"));
		::PostMessage(this->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	::PostMessage(this->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
}

void CBasicDemoSGDlg::OnCbnSelchangeCamListCombo()
{
	int nSel = m_cbCameraList.GetCurSel();
	if (nSel == CB_ERR)return;

	CString strSelected;
	m_cbCameraList.GetLBText(nSel, strSelected);

	CT2CA utf8Name(strSelected, CP_UTF8);
	std::string strCameraName(utf8Name);

	CAMERA cameraHandle = nullptr;
	// Cn:选择设备 | En:Select device by name
	for (const auto& pair : m_devMap) {
		const std::string& devName = pair.second.name;
		if (devName == strCameraName) {
			cameraHandle = pair.first;
		}
	}

	if (cameraHandle != nullptr) {
		// Cn:创建相机实例 | En:Create a Camera Instance
		m_pCamera = new VkCamera(cameraHandle);
	}
}


void CBasicDemoSGDlg::DisplayImage(unsigned char* pImagData, IMAGE_INFO stImageInfo) {
	CRect rc;
	m_wndDisplay->GetClientRect(&rc);
	int tmpx = rc.Height();
	int tmpy = rc.Width();
	int zoom_factor = 1;
	int all_data_size = stImageInfo.height*stImageInfo.width / zoom_factor;
	std::unique_ptr<unsigned char[]> zoom_char;
	zoom_char = std::unique_ptr<unsigned char[]>(new unsigned char[all_data_size]);
	if (stImageInfo.width / tmpx > 1 || stImageInfo.height / tmpy > 1)
	{
		unsigned char *original_data = pImagData;
		zoom_factor = stImageInfo.width / tmpx + 1;
		if (stImageInfo.height / tmpy > zoom_factor)
			zoom_factor = stImageInfo.height / tmpy + 1;

		all_data_size = stImageInfo.height*stImageInfo.width / zoom_factor / zoom_factor;
		unsigned char *zoom_tmp_data = zoom_char.get();
		for (int i = 0; i<stImageInfo.height / zoom_factor; ++i)
		{
			for (int j = 0; j<stImageInfo.width / zoom_factor; ++j)
			{
				*zoom_tmp_data = *original_data;
				zoom_tmp_data++;
				original_data += zoom_factor;
			}
			original_data += (stImageInfo.width*(zoom_factor - 1));
		}
		pImagData = zoom_char.get();
	}

	std::unique_ptr<unsigned char[]> one_char;
	one_char = std::unique_ptr<unsigned char[]>(new unsigned char[all_data_size * 3]);
	unsigned char *new_data = one_char.get();
	for (int i = 0; i<all_data_size; ++i)
	{
		*new_data = *pImagData;
		new_data++;
		*new_data = *pImagData;
		new_data++;
		*new_data = *pImagData;
		new_data++;
		pImagData++;
	}

	CDC* pDC = GetDlgItem(IDC_IMG_STATIC)->GetDC();
	BITMAPINFOHEADER bmiHeader;
	memset(&bmiHeader, 0, sizeof(BITMAPINFOHEADER));

	int width = stImageInfo.width / zoom_factor;
	int height = stImageInfo.height / zoom_factor;
	bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmiHeader.biWidth = width;
	bmiHeader.biHeight = height* (-1);
	bmiHeader.biPlanes = 1;
	bmiHeader.biBitCount = 24;
	bmiHeader.biCompression = BI_RGB;
	bmiHeader.biSizeImage = 0;

	unsigned char *show_data = one_char.get();
	if (pDC != nullptr)
	{
		SetDIBitsToDevice(pDC->m_hDC, 0, 0, width, height, 0, 0, 0, height, show_data,
			(PBITMAPINFO)&bmiHeader,
			DIB_RGB_COLORS);
		GetDlgItem(IDC_IMG_STATIC)->ReleaseDC(pDC);
	}
}

CString CBasicDemoSGDlg::SuffixToString(SAVE_IMAGE_TYPE type) const
{
	static const struct {
		SAVE_IMAGE_TYPE type;
		LPCTSTR suffix;
	} suffixMap[] = {
		{ Image_Raw,  _T(".raw") },
		{ Image_Bmp,  _T(".bmp") },
		{ Image_Png,  _T(".png") },
		{ Image_Jpeg, _T(".jpg") },
		{ Image_Tif,  _T(".tif") }
	};

	for (const auto& item : suffixMap) {
		if (item.type == type) {
			return item.suffix;
		}
	}

	return _T(".dat");
}

CString CBasicDemoSGDlg::GetSaveImagePath(SAVE_IMAGE_TYPE enSaveType)
{
	CString strDefaultName;
	strDefaultName.Format(_T("Image_w%d_h%d"),
		m_saveImgInfo.width,
		m_saveImgInfo.height);

	CString strSuffix = SuffixToString(enSaveType);

	CString strFilter;
	strFilter.Format(_T("%s Files (*%s)|*%s|All Files (*.*)|*.*||"),
		strSuffix.Mid(1).MakeUpper(), strSuffix, strSuffix);

	CFileDialog dlg(FALSE, strSuffix, strDefaultName,
		OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR,
		strFilter, this);
	dlg.m_ofn.lpstrTitle = _T("Save Image");

	if (!m_strLastSaveDir.IsEmpty()) {
		dlg.m_ofn.lpstrInitialDir = m_strLastSaveDir;
	}

	if (dlg.DoModal() != IDOK) {
		return _T("");
	}

	CString strPath = dlg.GetPathName();

	if (strPath.ReverseFind('.') < strPath.ReverseFind('\\')) {
		strPath += strSuffix;
	}

	m_strLastSaveDir = strPath.Left(strPath.ReverseFind('\\'));

	return strPath;
}

int CBasicDemoSGDlg::SaveImageToFile(SAVE_IMAGE_TYPE enSaveImageType) {
	int ret = OK;
	if (m_saveImageLen == 0 || m_saveImageBuf == nullptr) return -1;

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

	// Cn:获取保存路径 | En:Get save path
	CString strSavePath = GetSaveImagePath(enSaveImageType);
	if (!strSavePath.IsEmpty()) {
		// Cn:保存采集图像到文件 | En:Save image to file
		ret = m_pSGCard->SaveImage(&stImageInfo, &stImageParam, CT2A(strSavePath));
	}
	m_isSaving = false;
	return ret;
}