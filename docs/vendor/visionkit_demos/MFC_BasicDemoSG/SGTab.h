#pragma once
#include "afxdialogex.h"

class CBasicDemoSGDlg;
class VkCamera;

class CSGTab :
	public CDialogEx
{
public:
	CSGTab(CWnd* pParent = nullptr);
	~CSGTab();

	void SetParentDlg(CBasicDemoSGDlg* pDlg) { m_pParentDlg = pDlg; }							// Cn:设置父对话框引用 | En:Set parent dialog reference
	void SetDeviceController(VkCamera* pController) { m_pSGCardController = pController; }		// Cn:注入采集卡控制接口 | En:Inject SG card controller interface

	enum { IDD = IDD_SG_TAB };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	DECLARE_MESSAGE_MAP()
	virtual BOOL OnInitDialog();

	/* Cn:控件事件处理 | En:Control Event Handling */
	/* Cn:图像采集与保存 | En:Image Acquisition and Save */
	afx_msg void OnBnClickedStartGrabButton();			 // Cn:开始采集图像 | En:Start Grabbing
	afx_msg void OnBnClickedStopGrabButton();			 // Cn:停止采集图像 | En:Stop Grabbing
	afx_msg void OnBnClickedSaveRawButton();			 // Cn:保存Raw格式图像 | En:Save Raw	Format Image
	afx_msg void OnBnClickedSaveBmpButton();			 // Cn:保存Bmp格式图像 | En:Save Bmp	Format Image
	afx_msg void OnBnClickedSaveJpegButton();			 // Cn:保存Jpeg格式图像 | En:Save Jpeg Format Image
	afx_msg void OnBnClickedSaveTifButton();			 // Cn:保存Tif格式图像 | En:Save Tif Format Image
	afx_msg void OnBnClickedSavePngButton();			 // Cn:保存Png格式图像 | En:Save Png	Format Image

	/* Cn:参数获取与设置（采集卡） | En:Parameters Get and Set (SG Card) */
	afx_msg void OnCbnSelchangeClConfigCombo();				// Cn:设置Camera Link Config参数 | En:Set Camera Link Config Parameter
	afx_msg void OnCbnSelchangeCamColorTypeCombo();			// Cn:设置Camera Color Type参数 | En:Set Camera Color Type Parameter
	afx_msg void OnCbnSelchangeCamPixelDepthCombo();		// Cn:设置Camera Pixel Depth参数 | En:Set Camera Pixel Depth Parameter
	afx_msg void OnCbnSelchangeCamScanTypeCombo();			// Cn:设置Camera Scan Type参数 | En:Set Camera Scan Type Parameter
	afx_msg void OnCbnSelchangeDevTapGeometryCombo();		// Cn:设置Device Tap Geometry参数 | En:Set Device Tap Geometry Parameter
	afx_msg void OnCbnSelchangeCc1Combo();					// Cn:设置CC1参数 | En:Set CC1 Parameter
	afx_msg void OnCbnSelchangeLineTriEnableCombo();		// Cn:设置Line Trigger Enable参数 | En:Set Line Trigger Enable Parameter
	afx_msg void OnCbnSelchangeLineTriSourceCombo();		// Cn:设置Line Trigger Source参数 | En:Set Line Trigger Source Parameter
	afx_msg void OnBnClickedLineTriSoftwareButton();		// Cn:执行Line Trigger Software命令 | En:Execute Line Trigger Software Command
	afx_msg void OnCbnSelchangeFrameTriEnableCombo();		// Cn:设置Frame Trigger Enable参数 | En:Set Frame Trigger Enable Parameter
	afx_msg void OnCbnSelchangeFrameTriSourceCombo();		// Cn:设置Frame Trigger Source参数 | En:Set Frame Trigger Source Parameter
	afx_msg void OnBnClickedFrameTriSoftwareButton();		// Cn:设置Frame Internal Trigger Rate参数 | En:Set Frame Internal Trigger Rate Parameter
	afx_msg void OnBnClickedSgGetParamButton();				// Cn:获取采集卡参数 | En:Get SG Card Parameter
	afx_msg void OnBnClickedSgConfigSaveButton();			 // Cn:保存采集卡配置参数文件 | En:Save Config File
	afx_msg void OnBnClickedSgConfigLoadButton();			 // Cn:加载采集卡配置参数文件 | En:Load Config File

	/* Cn:键盘事件处理 | En:Keyboard Event Handling */
	virtual BOOL PreTranslateMessage(MSG* pMsg);
	void OnApplyHorizontal();
	void OnApplyVertical();
	void OnApplyLineTriRate();
	void OnApplyFrameTriRate();

private:
	/* Cn:基础参数控件 | En:Basic parameter controls */
	CComboBox	m_boxCLConfig;				// Cn:Camera Link配置下拉框 | En:Camera Link config combo
	CComboBox	m_boxCamColorType;			// Cn:Color type下拉框 | En:Color type combo
	CComboBox	m_boxCamPixelDepth;			// Cn:Pixel depth下拉框 | En:Pixel depth combo
	CComboBox	m_boxCamScanType;			// Cn:Scan type下拉框 | En:Scan type combo
	CComboBox	m_boxDevTapGeometry;		// Cn:Tap geometry下拉框 | En:Tap geometry combo
	CEdit		m_editHorizontal;			// Cn:水平参数编辑框 | En:Horizontal parameter edit
	CEdit		m_editVertical;				// Cn:垂直参数编辑框 | En:Vertical parameter edit

	/* Cn:触发控制控件 | En:Trigger controls */
	CComboBox	m_boxCC1;					// Cn:CC1参数下拉框 | En:CC1 parameter combo
	CComboBox	m_boxLineTriEnable;			// Cn:行触发使能下拉框 | En:Line trigger enable combo
	CComboBox	m_boxLineTriSource;			// Cn:行触发源下拉框 | En:Line trigger source combo
	CEdit		m_editLineTriRate;			// Cn:行内触发率编辑框 | En:Line trigger rate edit
	CButton		m_btnLineTriSoftware;		// Cn:软件行触发按钮 | En:Software line trigger button
	CComboBox	m_boxFrameTriEnable;		// Cn:帧触发使能下拉框 | En:Frame trigger enable combo
	CComboBox	m_boxFrameTriSource;		// Cn:帧触发源下拉框 | En:Frame trigger source combo
	CEdit		m_editFrameTriRate;			// Cn:帧内触发率编辑框 | En:Frame trigger rate edit
	CButton		m_btnFrameTriSoftware;		// Cn:软件帧触发按钮 | En:Software frame trigger button

private:
	CBasicDemoSGDlg* m_pParentDlg;			// Cn:父对话框指针 | En:Parent dialog pointer
	VkCamera* m_pSGCardController;			// Cn:采集卡控制接口（由父对话框注入） | En:SG card control interface (injected by parent dialog)

private:
	void setupEnumComboBox(const char* feature,
		CComboBox* box, const CString& errorMsg);			// Cn:配置ComboBox控件子项 | En:Configuring ComboBox control sub-items
};

