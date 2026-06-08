// pages/bind/bind.js
const app = getApp();

Page({
  data: {
    bindCode: '',
    binding:  false,
    resultMsg: '',
    resultOk:  false,
  },

  onCodeInput(e) {
    this.setData({ bindCode: e.detail.value.toUpperCase(), resultMsg: '' });
  },

  bindDevice() {
    const code = this.data.bindCode.trim().toUpperCase();
    if (code.length !== 6) return;

    this.setData({ binding: true, resultMsg: '' });

    // Get openid via wx.login first
    wx.login({
      success: (loginRes) => {
        // In production: exchange loginRes.code for openid via your server
        // For demo: use code directly as a temporary openid
        const openid = loginRes.code;
        app.globalData.openid = openid;
        wx.setStorageSync('openid', openid);

        wx.request({
          url:    `${app.globalData.serverUrl}/api/bind`,
          method: 'POST',
          header: { 'content-type': 'application/x-www-form-urlencoded' },
          data:   `bind_code=${code}&openid=${openid}`,
          success: (res) => {
            if (res.statusCode === 200 && res.data.success) {
              const deviceId = res.data.device_id;
              app.globalData.deviceId = deviceId;
              wx.setStorageSync('deviceId', deviceId);
              this.setData({
                resultMsg: `✅ 绑定成功！相框ID：${deviceId}`,
                resultOk:  true,
              });
              // Return to main page after 1.5s
              setTimeout(() => wx.navigateBack(), 1500);
            } else {
              this.setData({
                resultMsg: `绑定失败：${res.data.detail || '绑定码错误'}`,
                resultOk:  false,
              });
            }
          },
          fail: () => {
            this.setData({ resultMsg: '网络错误，请重试', resultOk: false });
          },
          complete: () => {
            this.setData({ binding: false });
          },
        });
      },
      fail: () => {
        this.setData({ binding: false, resultMsg: '登录失败，请重试', resultOk: false });
      },
    });
  },
});
