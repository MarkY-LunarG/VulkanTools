<!-- markdownlint-disable MD041 -->
<p align="left"><img src="https://vulkan.lunarg.com/img/NewLunarGLogoBlack.png" alt="LunarG" width=263 height=113 /></p>
<p align="left">Copyright &copy; 2023 LunarG, Inc.</p>

[![Creative Commons][3]][4]

[3]: https://i.creativecommons.org/l/by-nd/4.0/88x31.png "Creative Commons License"
[4]: https://creativecommons.org/licenses/by-nd/4.0/

# VK\_LAYER\_LUNARG\_slow\_device\_simulator
The `VK_LAYER_LUNARG_slow_device_simulator` layer slows down responses for
VkFence results as if the underlying device was slower than it is.

## Configuring the Slow Device Simulator Layer

For an overview of how to configure layers, refer to the
[Layers Overview and Configuration](https://vulkan.lunarg.com/doc/sdk/latest/windows/layer_configuration.html)
document.

The Slow Device Simulator Layer settings are documented in the
[Layer Details](https://vulkan.lunarg.com/doc/sdk/latest/windows/slow_device_simulator_layer.html#layer-options)
section below.

The Slow Device Simulator Layer can also be enabled and configured using
vkconfig.
See the [vkconfig](https://vulkan.lunarg.com/doc/sdk/latest/windows/vkconfig.html)
documentation for more information.


## Enabling the Slow Device Simulator Layer

### Desktop (Linux/Windows/MacOS)

You must add the location of the `VkLayer_slow_device_simmulator.json` file and
corresponding `VkLayer_slow_device_simulator` library to your `VK_LAYER_PATH`
in order for the Vulkan loader to be able to find the layer.

Then, you must also enable the layer in one of two ways:

 * Directly in your application using the layer's name during `vkCreateInstance`
 * Indirectly by using the `VK_INSTANCE_LAYERS` environment variable.

#### Setting `VK_LAYER_PATH`

**Windows**

If your source was located in: `C:\my_folder\vulkantools` and your build folder
was build64, then you would add it to the layer path in the following way:

    set VK_LAYER_PATH=C:\my_folder\vulkantools\build64\layersvt\Debug;%VK_LAYER_PATH%

**Linux/MacOS**

If your source was located in: `/my_folder/vulkantools` and your build folder
was build, then you would add it to the layer path in the following way:

    export VK_LAYER_PATH=/my_folder/vulkantools/build/layersvt:$VK_LAYER_PATH

#### Enabling the layer with `VK_INSTANCE_LAYERS`

To force the layer to be enabled for Vulkan applications, you can set the
`VK_INSTANCE_LAYERS` environment variable in the following way:

**Windows**

    set VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_slow_device_simulator

**Linux/MacOS**

    export VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_slow_device_simulator

<br></br>

### Android

#### Globally Enabling the Layer

Use ADB to enable the layer for your project by:

    adb shell "setprop debug.vulkan.layers 'VK_LAYER_LUNARG_slow_device_simulator'"

When done, disable the layer using:

    adb shell "setprop debug.vulkan.layers ''"

<br></br>

### Applying Environment Settings on Android

On Android, you must use properties to set the layer environment variables.
The format of the properties to set takes the following form:

    debug. + (lower-case environment variable with 'vk_' stripped)

The easiest way to set a property is from the ADB shell:

    adb shell "setprop <property_name> '<property_value>'"

**For example:**

To set the memory percent, which on desktop uses `VK_SLOWDEVICESIM_MEMORY_PERCENT`
set the following property:

    debug.slowdevicesim_memory_percent

Which you can set in the following way:

    adb shell "setprop debug.slowdevicesim_memory_percent '80'"

<br></br>


## Layer Options

The options for this layer are specified in VkLayer_slow_device_simulator.json,
but are listed below for convenience:

<table style="width:100%">
  <tr>
    <th>Layer Option</th>
    <th>Description</th>
    <th>Environment Variable Name</th>
    <th>Android Property Name</th>
    <th>Setting File Parameter</th>
    <th>Possible Values</th>
  </tr>
  <tr>
    <td>Fence Delay Type</td>
    <td><small>Specify the way to delay a fence</small></td>
    <td><small>VK_SLOWDEVICESIM_<br/>FENCE_DELAY_TYPE</small></td>
    <td><small>debug.slowdevicesim_<br/>fence_delay_type</small></td>
    <td><small>{LayerIdentifier}.<br/>fence_delay_type</small></td>
    <td><small>
        <b>none</b> - Disable the fence delay<br/>
        <b>ms_from_trigger</b> - Wait x milliseconds from the event triggering the fence<br/>
        <b>ms_from_first_query</b> - Wait x milliseconds from the first fence wait query<br/>
        <b>num_fail_waits</b> - Indicate for x waits that fence is not ready before responding with success<br/>
    </small></td>
  </tr>
  <tr>
    <td>Fence Delay Count</td>
    <td><small>The count to wait, based on <i>fence_delay_type</i></small></td>
    <td><small>VK_SLOWDEVICESIM_<br/>FENCE_DELAY_COUNT</small></td>
    <td><small>debug.slowdevicesim_<br/>fence_delay_count</small></td>
    <td><small>{LayerIdentifier}.<br/>fence_delay_count</small></td>
    <td><small>uint32_t value (either ms or count)</small></td>
  </tr>
  <tr>
    <td>Memory Percent</td>
    <td><small>The percentage of actual memory to report back up (<=100)</small></td>
    <td><small>VK_SLOWDEVICESIM_<br/>MEMORY_PERCENT</small></td>
    <td><small>debug.slowdevicesim_<br/>memory_percent</small></td>
    <td><small>{LayerIdentifier}.<br/>memory_percent</small></td>
    <td><small>uint32_t value 1-100</small></td>
  </tr>
</table>

**NOTE** {LayerIdentifier} = "lunarg_slow_device_simulator"
