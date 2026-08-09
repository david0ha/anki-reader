import { GestureHandlerRootView } from 'react-native-gesture-handler'
import { SafeAreaProvider } from 'react-native-safe-area-context'
import { Stack } from 'expo-router'
import { DeviceProvider } from '../lib/device'
import { colors } from '../theme'

export default function RootLayout() {
  return (
    <GestureHandlerRootView style={{ flex: 1 }}>
      <SafeAreaProvider>
        {/* DeviceProvider resolves the control-API base URL once and shares a client app-wide. */}
        <DeviceProvider>
          <Stack
            screenOptions={{
              headerShown: false,
              contentStyle: { backgroundColor: colors.bg },
            }}
          />
        </DeviceProvider>
      </SafeAreaProvider>
    </GestureHandlerRootView>
  )
}
