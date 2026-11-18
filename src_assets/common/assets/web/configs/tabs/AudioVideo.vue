<script setup>
import {ref} from 'vue'
import {$tp} from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";
import Checkbox from "../../Checkbox.vue";

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input type="text" class="form-control" id="audio_sink"
             :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
             v-model="config.audio_sink" />
      <div class="form-text">
        {{ $tp('config.audio_sink_desc') }}<br>
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #freebsd>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>


    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input type="text" class="form-control" id="virtual_sink" :placeholder="$t('config.virtual_sink_placeholder')"
                 v-model="config.virtual_sink" />
          <div class="form-text">{{ $t('config.virtual_sink_desc') }}</div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <Checkbox class="mb-3"
                  id="install_steam_audio_drivers"
                  locale-prefix="config"
                  v-model="config.install_steam_audio_drivers"
                  default="true"
        ></Checkbox>
      </template>
    </PlatformLayout>

    <!-- Audio Delay -->
    <div class="mb-3">
      <label for="audio_delay" class="form-label">{{ $t('config.audio_delay') }}</label>
      <input type="number" class="form-control" id="audio_delay" min="0" max="500" step="5"
             placeholder="0" v-model="config.audio_delay" />
      <div class="form-text">{{ $t('config.audio_delay_desc') }}</div>
    </div>

    <PlatformLayout :platform="platform">
      <template #macos>
        <!-- Client Microphone -->
        <Checkbox class="mb-3"
                  id="client_mic"
                  locale-prefix="config"
                  v-model="config.client_mic"
                  default="true"
        ></Checkbox>

        <!-- Client Microphone as Default Input -->
        <Checkbox class="mb-3"
                  id="client_mic_default_input"
                  locale-prefix="config"
                  v-model="config.client_mic_default_input"
                  default="true"
        ></Checkbox>

        <!-- Client Microphone Voice Isolation -->
        <Checkbox class="mb-3"
                  id="client_mic_voice_isolation"
                  locale-prefix="config"
                  v-model="config.client_mic_voice_isolation"
                  default="true"
        ></Checkbox>

        <!-- Client Microphone Level -->
        <div class="mb-3">
          <label for="client_mic_gain" class="form-label">{{ $t('config.client_mic_gain') }}</label>
          <input type="number" class="form-control" id="client_mic_gain" min="-30" max="12" step="1"
                 placeholder="4" v-model="config.client_mic_gain" />
          <div class="form-text">{{ $t('config.client_mic_gain_desc') }}</div>
        </div>

        <!-- Client Microphone Echo Cancellation -->
        <Checkbox class="mb-3"
                  id="client_mic_echo_cancellation"
                  locale-prefix="config"
                  v-model="config.client_mic_echo_cancellation"
                  default="true"
        ></Checkbox>

        <!-- Client Microphone Voice Equalizer -->
        <Checkbox class="mb-3"
                  id="client_mic_eq"
                  locale-prefix="config"
                  v-model="config.client_mic_eq"
                  default="true"
        ></Checkbox>
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <Checkbox class="mb-3"
              id="stream_audio"
              locale-prefix="config"
              v-model="config.stream_audio"
              default="true"
    ></Checkbox>

    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Externally Managed Audio -->
        <Checkbox class="mb-3"
                  id="external_audio"
                  locale-prefix="config"
                  v-model="config.external_audio"
                  default="false"
        ></Checkbox>
      </template>
    </PlatformLayout>

    <AdapterNameSelector
        :platform="platform"
        :config="config"
    />

    <DisplayOutputSelector
      :platform="platform"
      :config="config"
    />

    <PlatformLayout :platform="platform">
      <template #macos>
        <!-- Per-client virtual display -->
        <Checkbox class="mb-3"
                  id="macos_virtual_display"
                  locale-prefix="config"
                  v-model="config.macos_virtual_display"
                  default="true"
        ></Checkbox>
        <Checkbox class="mb-3"
                  id="macos_virtual_display_hidpi"
                  locale-prefix="config"
                  v-model="config.macos_virtual_display_hidpi"
                  default="true"
        ></Checkbox>
        <div class="mb-3">
          <label for="macos_virtual_display_scale" class="form-label">{{ $t('config.macos_virtual_display_scale') }}</label>
          <select id="macos_virtual_display_scale" class="form-select" v-model="config.macos_virtual_display_scale">
            <option value="75">{{ $t('config.macos_virtual_display_scale_larger', { percent: 75 }) }}</option>
            <option value="85">{{ $t('config.macos_virtual_display_scale_larger', { percent: 85 }) }}</option>
            <option value="90">{{ $t('config.macos_virtual_display_scale_larger', { percent: 90 }) }}</option>
            <option value="100">{{ $t('config.macos_virtual_display_scale_default') }}</option>
            <option value="115">{{ $t('config.macos_virtual_display_scale_more_space', { percent: 115 }) }}</option>
            <option value="130">{{ $t('config.macos_virtual_display_scale_more_space', { percent: 130 }) }}</option>
            <option value="150">{{ $t('config.macos_virtual_display_scale_more_space', { percent: 150 }) }}</option>
            <option value="175">{{ $t('config.macos_virtual_display_scale_more_space', { percent: 175 }) }}</option>
            <option value="200">{{ $t('config.macos_virtual_display_scale_more_space', { percent: 200 }) }}</option>
          </select>
          <div class="form-text">{{ $t('config.macos_virtual_display_scale_desc') }}</div>
        </div>
        <Checkbox class="mb-3"
                  id="macos_virtual_display_primary"
                  locale-prefix="config"
                  v-model="config.macos_virtual_display_primary"
                  default="true"
        ></Checkbox>
        <Checkbox class="mb-3"
                  id="macos_virtual_display_disable_host"
                  locale-prefix="config"
                  v-model="config.macos_virtual_display_disable_host"
                  default="false"
        ></Checkbox>
        <Checkbox class="mb-3"
                  id="macos_stop_stream_hotkey"
                  locale-prefix="config"
                  v-model="config.macos_stop_stream_hotkey"
                  default="true"
        ></Checkbox>
        <Checkbox class="mb-3"
                  id="macos_block_local_input"
                  locale-prefix="config"
                  v-model="config.macos_block_local_input"
                  default="true"
        ></Checkbox>
      </template>
    </PlatformLayout>

    <DisplayDeviceOptions
      :platform="platform"
      :config="config"
    />

    <!-- Display Modes -->
    <DisplayModesSettings
        :platform="platform"
        :config="config"
    />

  </div>
</template>
