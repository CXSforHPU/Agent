/**
 * Naive UI 主题配置。品牌色、圆角、字号都在这里改一处即可。
 */

export const brandColor = '#2f6bff'

export const themeOverrides = {
  common: {
    primaryColor: brandColor,
    primaryColorHover: '#4d80ff',
    primaryColorPressed: '#1f56e0',
    primaryColorSuppl: '#4d80ff',
    infoColor: brandColor,
    infoColorHover: '#4d80ff',
    infoColorPressed: '#1f56e0',
    successColor: '#12a150',
    successColorHover: '#2fb56a',
    successColorPressed: '#0b8a42',
    warningColor: '#f0a020',
    warningColorHover: '#f5b64c',
    warningColorPressed: '#d98c12',
    errorColor: '#e5484d',
    errorColorHover: '#ea6b6f',
    errorColorPressed: '#ce3b40',
    borderRadius: '8px',
    borderRadiusSmall: '6px',
    fontSize: '14px',
    fontSizeSmall: '13px',
    fontFamily:
      '"Microsoft YaHei", "PingFang SC", system-ui, -apple-system, "Segoe UI", sans-serif',
  },
  Card: {
    borderRadius: '12px',
    borderColor: '#e6e9ef',
    paddingMedium: '18px 20px',
    color: '#ffffff',
  },
  Layout: {
    siderColor: '#141b2d',
    siderColorActive: '#1d2740',
    siderBorderColor: '#141b2d',
    siderColorCollapsed: '#141b2d',
    headerColor: '#ffffff',
    headerBorderColor: '#e6e9ef',
    headerHeight: '58px',
  },
  Menu: {
    itemHeight: '42px',
    borderRadius: '8px',
    fontSize: '14px',
  },
  Button: {
    borderRadiusMedium: '8px',
    fontWeight: '500',
  },
  Input: {
    borderRadius: '8px',
  },
  Select: {
    peers: {
      InternalSelection: {
        borderRadius: '8px',
      },
    },
  },
  DataTable: {
    thColor: '#f7f8fa',
    thTextColor: '#5b6478',
    thPadding: '10px 12px',
    tdPadding: '12px',
    borderColor: '#eef0f4',
    borderRadius: '12px',
  },
  Tag: {
    borderRadius: '6px',
  },
  DataTableInternal: {
    borderRadius: '12px',
  },
  Modal: {
    borderRadius: '14px',
  },
  Empty: {
    padding: '40px 0',
  },
  Descriptions: {
    thColor: '#f7f8fa',
  },
  Progress: {
    fillColor: brandColor,
  },
}
