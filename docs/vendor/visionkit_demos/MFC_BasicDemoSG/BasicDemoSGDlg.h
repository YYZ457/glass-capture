#pragma once
#include "VkCamera.h"
#include "SGTab.h"
#include "CameraTab.h"

// Cn:自定义消息定义 | En:Custom Message Definitions
#define WM_UPDATE_IMAGE			(WM_USER + 100)
#define WM_UPDATE_STATISTICS	(WM_USER + 101)
#define WM_HEARTBEAT_TIMEOUT	(WM_USER + 102)
#define WM_UPDATE_UI			(WM_USER + 103)
#define WM_SHOW_ERROR_MSG		(WM_USER + 104)

#define WM_SG_SAVE_RAW			(WM_USER + 200)
#define WM_SG_SAVE_BMP			(WM_USER + 201)
#define WM_SG_SAVE_JPEG			(WM_USER + 202)
#define WM_SG_SAVE_TIF			(WM_USER + 203)
#define WM_SG_SAVE_PNG			(WM_USER + 204)

struct ImageDisplayData {
	unsigned char* pImgData;	// Cn:图像数据指针 | En:Image data pointer
	IMAGE_INFO stImgInfo;		// Cn:图像信息 | En:Image Information
};

struct Statistics {
	unsigned int nImgCnt;				// Cn:接收帧数 | En:Images Count
	unsigned int nImgErrCnt;			// Cn:错误帧数 | En:Images Error Count
	unsigned int nImgLostCnt;			// Cn:丢失帧数 | En:Images Lost Count
	unsigned int nSegmentsLostCnt;		// Cn:丢失包数 | En:Segments Lost Count
	unsigned int nSegmentsResendCnt;	// Cn:重传包数 | En:Segments Resend Count
};


class CBasicDemoSGDlg : public CDialogEx
{
public:
	explicit CBasicDemoSGDlg(CWnd* pParent = nullptr);
	virtual ~CBasicDemoSGDlg();

	VkCamera* GetDevController() const { return m_pSGCard; }	// Cn:获取当前采集卡控制器实例 | En: Get active capture card controller instance

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_BASICDEMOSG_DIALOG };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);

public:
	IMAGE_INFO			m_saveImgInfo;	                        // Cn:图像信息 | En:Image Data Length
	unsigned char*		m_saveImageBuf;	                        // Cn:图像数据缓存（用于临时存储待保存的原始图像数据） | En:Image Data Cache (used for temporarily storing raw image data to be saved)
	uint32_t			m_saveImageLen;	                        // Cn:图像数据长度 | En:Image Data Length
	bool				m_isSaving;		                        // Cn:图像保存状态标志 | En:Image Save Status Flag

protected:
	DECLARE_MESSAGE_MAP()
	virtual BOOL OnInitDialog();
	afx_msg HCURSOR OnQueryDragIcon();

	/* Cn:自定义消息处理 | En:Custom message handling */
	afx_msg LRESULT OnDisplayImage(WPARAM wParam, LPARAM lParam);		// Cn:显示图像 | En:Display Image
	afx_msg LRESULT OnUpdateStats(WPARAM wParam, LPARAM lParam);		// Cn:更新图像统计信息 | En:Update Image Statistics
	afx_msg LRESULT OnDevConnectLost(WPARAM wParam, LPARAM lParam);		// Cn:处理相机失联事件 | En:Handle Camera Connection Loss Event
	afx_msg	LRESULT OnUpdateUI(WPARAM wParam, LPARAM lParam);			// Cn:更新界面控件状态 | En:Update Interface Control Status
	afx_msg	LRESULT OnShowErrorMsg(WPARAM wParam, LPARAM lParam);		// Cn:反馈错误信息 | En:Show Error Message

	afx_msg	LRESULT OnSaveRaw(WPARAM wParam, LPARAM lParam);			// Cn:保存Raw格式图像 | En:Save Raw	Format Image
	afx_msg	LRESULT OnSaveBmp(WPARAM wParam, LPARAM lParam);			// Cn:保存Bmp格式图像 | En:Save Bmp	Format Image
	afx_msg	LRESULT OnSaveJpeg(WPARAM wParam, LPARAM lParam);			// Cn:保存Jpeg格式图像 | En:Save Jpeg Format Image
	afx_msg	LRESULT OnSaveTif(WPARAM wParam, LPARAM lParam);			// Cn:保存Tif格式图像 | En:Save Tiff Format Image
	afx_msg	LRESULT OnSavePng(WPARAM wParam, LPARAM lParam);			// Cn:保存Png格式图像 | En:Save Png	Format Image

	/* Cn:控件事件处理 | En:Control Event Handling */
	afx_msg void OnTcnSelchangeDevTab(NMHDR *pNMHDR, LRESULT *pResult); // Cn:处理设备标签页切换事件 | En:Handle device tab switching events
	afx_msg void OnBnClickedEnumButton();			                    // Cn:枚举所有可用采集卡与相机  | En:Enumerate All Available SG Cards and Cameras
	afx_msg void OnBnClickedSgOpenButton();			                    // Cn:连接选择的采集卡 | En:Connect SG Card
	afx_msg void OnBnClickedSgCloseButton();		                    // Cn:关闭选择的采集卡 | En:Disconnect SG Card
	afx_msg void OnCbnSelchangeSgListCombo();		                    // Cn:通过名称创建新采集卡实例 | En:Create a New SG Card Instance by Name
	afx_msg void OnBnClickedCamOpenButton();		                    // Cn:连接选择的相机 | En:Connect Camera
	afx_msg void OnBnClickedCamCloseButton();		                    // Cn:关闭选择的相机 | En:Disconnect Camera
	afx_msg void OnCbnSelchangeCamListCombo();		                    // Cn:通过名称创建新相机实例 | En:Create a New Camera Instance by Name

private:
	CStatic				m_wndStatsCtrl;		// Cn:统计信息控件 | En:Statistics Display Control
	CStatic				m_wndPictureCtrl;	// Cn:绘图操作目标窗口 | En:Target Window for Drawing Operations
	CWnd*				m_wndDisplay;		// Cn:底层绘图窗口指针，用于直接GDI操作 | En:Low-level Drawing Window Pointer for Direct GDI Operations

	CTabCtrl			m_tabCtrl;			// Cn:主标签页控件容器 | En:Main Tab Control Container
	CSGTab				m_tabSGCard;		// Cn:采集卡控制子页面 | En:Capture Card Control Subpage
	CCameraTab			m_tabCamera;		// Cn:相机控制子页面 | En:Camera Control Subpage

	CComboBox			m_cbSGCardList;		// Cn:采集卡列表组合框 | En:SG Card List Combo Box
	CComboBox			m_cbCameraList;		// Cn:相机列表组合框 | En:Camera List Combo Box

private:
	VkCamera*			m_pSGCard;			// Cn:采集卡控制器实例（封装SDK底层操作） | En:SG Card Controller Example (Encapsulating SDK Underlying Operations)
	VkCamera*			m_pCamera;		    // Cn:相机控制器实例 | En:Camera Controller Instance
	DeviceMap			m_devMap;			    // Cn:设备映射表（存储枚举到的设备） | En:Device Mapping Table (Storage for enumerated devices)

	CString				m_strLastSaveDir;   // Cn:最后保存目录 | En:Last Save Directory

	Statistics			m_stats;			// Cn:统计信息 | En:Statistics
	CCriticalSection	m_csStats;			// Cn:统计信息同步锁 | En:Statistics Synchronization Lock

	HICON				m_hIcon;

private:
	/* Cn:初始化 | En:Initialization */
	void Init();															// Cn:初始化软件 | En:Initializing Software
	void SetupTab();														// Cn:初始化并配置标签页控件 | En:Initialize and Configure Tab Control

	/* Cn:界面更新 | En:Interface Update */
	void DisplayImage(unsigned char* pImagData, IMAGE_INFO stImageInfo);	// Cn:显示图像 | En:Display Image
	
	/* Cn:工具函数 | En:Utility functions */
	CString SuffixToString(SAVE_IMAGE_TYPE enSaveImageType) const;			// Cn:生成图像文件尾缀 | En:Generate Image File Suffix
	CString GetSaveImagePath(SAVE_IMAGE_TYPE enSaveImageType);				// Cn:获取图像保存路径 | En:Get Image Save Path
	int SaveImageToFile(SAVE_IMAGE_TYPE enSaveImageType);					// Cn:保存图像到指定路径 | En:Save Image to Specified Path	

};
