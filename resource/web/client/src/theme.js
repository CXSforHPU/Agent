/**
 * Naive UI 主题配置。品牌色、圆角、字号都在这里改一处即可。
 */

export const brandColor = '#6366f1'

export const themeOverrides = {
  common: {
    primaryColor: brandColor,
    primaryColorHover: '#8183f5',
    primaryColorPressed: '#4f46e5',
    primaryColorSuppl: '#8183f5',
    infoColor: brandColor,
    infoColorHover: '#8183f5',
    infoColorPressed: '#4f46e5',
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
    borderRadius: '16px',
    borderColor: '#e6e9ef',
    paddingMedium: '24px',
    color: '#ffffff',
  },
  Layout: {
    siderColor: '#ffffff',
    siderColorActive: '#eef0ff',
    siderBorderColor: '#eaecf3',
    siderColorCollapsed: '#ffffff',
    headerColor: '#ffffff',
    headerBorderColor: '#e6e9ef',
    headerHeight: '68px',
  },
  Menu: {
    itemHeight: '46px',
    itemTextColor: '#667085',
    itemIconColor: '#8a91a6',
    itemColorActive: '#eeefff',
    itemColorActiveHover: '#e6e8ff',
    itemTextColorActive: '#5759d6',
    itemIconColorActive: '#5759d6',
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
    borderRadius: '16px',
  },
  Tag: {
    borderRadius: '6px',
  },
  DataTableInternal: {
    borderRadius: '16px',
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
