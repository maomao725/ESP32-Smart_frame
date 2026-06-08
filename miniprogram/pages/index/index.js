// pages/index/index.js
const app = getApp();

Page({
  data: {
    previewSrc:  '',
    sending:     false,
    resultMsg:   '',
    resultOk:    false,
    frameOnline: false,
  },

  onShow() {
    this._checkFrameStatus();
  },

  // ── 选择照片 ──────────────────────────────────────────────────
  choosePhoto() {
    wx.chooseMedia({
      count: 1,
      mediaType: ['image'],
      sourceType: ['album', 'camera'],
      success: (res) => {
        this.setData({
          previewSrc: res.tempFiles[0].tempFilePath,
          resultMsg:  '',
        });
      },
    });
  },

  // ── 发送照片 ──────────────────────────────────────────────────
  sendPhoto() {
    const openid   = app.globalData.openid;
    const deviceId = app.globalData.deviceId;

    if (!openid || !deviceId) {
      wx.showModal({
        title: '提示',
        content: '请先绑定相框',
        showCancel: false,
        success: () => wx.navigateTo({ url: '/pages/bind/bind' }),
      });
      return;
    }

    this.setData({ sending: true, resultMsg: '' });

    wx.uploadFile({
      url:      `${app.globalData.serverUrl}/api/upload`,
      filePath: this.data.previewSrc,
      name:     'file',
      formData: { openid },
      success: (res) => {
        const data = JSON.parse(res.data);
        if (data.success) {
          this.setData({ resultMsg: '✅ 照片已送达，奶奶马上就能看到！', resultOk: true });
        } else {
          this.setData({ resultMsg: `发送失败：${data.detail || '未知错误'}`, resultOk: false });
        }
      },
      fail: () => {
        this.setData({ resultMsg: '网络错误，请检查网络后重试', resultOk: false });
      },
      complete: () => {
        this.setData({ sending: false });
      },
    });
  },

  // ── 查询相框在线状态 ──────────────────────────────────────────
  _checkFrameStatus() {
    const deviceId = app.globalData.deviceId;
    if (!deviceId) return;

    wx.request({
      url:     `${app.globalData.serverUrl}/api/status/${deviceId}`,
      method:  'GET',
      success: (res) => {
        if (res.statusCode === 200) {
          this.setData({ frameOnline: res.data.online });
        }
      },
    });
  },

  goToBind() {
    wx.navigateTo({ url: '/pages/bind/bind' });
  },
});
