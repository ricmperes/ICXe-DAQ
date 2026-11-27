# About the project:

In this project we want to develop a DAQ system for a small liquid xenon time projection chamber.

## The detector:

The TPC is equipped with an array of 64 Silicon Photomultiplier Tubes (SiPMs) at the top and 4 regular Photomultiplier tubes (PMTs) at the bottom.

Out of the 64 SiPMs, 8 are broken.

The main signal we want to measure are S1 and S2 signals. These signals are typically seperated by a certain drift time that depends linearly on the depth where the signal occured in the detector.

The TPC has a length of 8 cm and expected to have a maximum drift time of about 50 µs to 60 µs. Shorter distances have shoter drift times. 

A typical S1 signal is expected to be about 1 µs long.

A typical S2 signal is expected to be 2 µs to 3 µs long. 

It is expected that the SiPM array will have a dark count rate of about 5 Hz to 10 Hz per device.

The dark count rate of the PMTs is expected to be abut 2.5 Hz.

We might want to record additional signals from another external detector. The detector may use one or two channels.

## The DAQ system

The DAQ consistet of 5 V1730SB digitizer, V4718 VME bridge and a VME crate. All of the manufacturer Caen.

The digitizer have a 14 bit resolution over a dynamic range of 2 Vpp. The sampeling frequency is 500 Mhz.

# What the DAQ system should be able to provide:

The DAQ should be able to read out all channels in a synchronized way. The number of channels that are read out should be configurable.

The DAQ should be able to start recording based on an external trigger, but also letting each channel running independently based on a channel specific trigger threshold.

Here are some example run modes the DAQ should support:

1. Single or any number of sensors externally triggered. 
2. Single or any number of sensors self-triggered.
3. The TPC also should be operatable in these two modes. 
4. Overlapping recording windows should be concatenated. If an S1 and S2 appear shortly one after another recording windows may overlap and we want to record both signals.
5. The output should provide a binary file containing the data as well as timestamps, and a metadata file containing information about each binary file (chunk).
6. A chunk should be self-contained. This means: 
    1. Each chunk comes with a start and endtime in unix time ns
    2. There must be no gaps between chunks.
    3. The data within a chunk must be time and channel sorted, no waveform is allowed to reach beyond the boundaries of a chunk.

# Examples and resources:

An example for a similar system is the redax system which can be found on github AxFoundation/redax.

The DAQ system should enable a processing of the data using the strax framework, see AxFoundation/strax.


# Development rules for this project

* Config keys: every key of the run config is required (no hidden defaults). Whenever a config key is added, changed or removed, update docs/config.md in the same change: type, allowed values, and a "What it does" text that says what the key does to the board or the run, which register it sets, when it has an effect, and what `null` / `[]` means for it. Also add the key to every file in config/ and to the self-test fixture (`full_config` in src/self_test.cpp).
