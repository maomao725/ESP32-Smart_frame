// app.js
App({
  globalData: {
    serverUrl: 'http://YOUR_SERVER_IP:8000',  // 替换为腾讯云服务器IP
    openid: '',
    deviceId: '',
  },

  onLaunch() {
    const openid = wx.getStorageSync('openid');
    const deviceId = wx.getStorageSync('deviceId');
    if (openid) this.globalData.openid = openid;
    if (deviceId) this.globalData.deviceId = deviceId;
  },
});
