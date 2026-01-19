# gbs2midi
Convert GBS (Game Boy music file) to midi files.

Please see this [video for a demonstration of what gbs2midi is capable of](https://www.youtube.com/watch?v=nB5xkJSd_8M).

The gbsplay executable must be in the same folder as the gbs2midi executable for the program to work.

The midi files exported by this program are meant to be used with my [LV2 and CLAP synthesizer plugin Nelly GB](https://github.com/Thysbelon/Nelly-GB-synth), which converts the midi events back into Game Boy APU register writes and renders the audio using an emulated Game Boy APU.  
This makes it possible to play back and edit Game Boy music in a way that sounds accurate to the original.

## Usage Tips

### Tempo

Output midi files do not follow any tempo. To work with these files, I recommend that you open the midi files in a DAW and set the midi files to ignore the DAW's tempo.    
Here is how to do that in Reaper:   
1. Leave your Reaper project at the default BPM of 120
2. Open the midi file in Reaper
3. Select all midi items (e.g. by shift-clicking), open the right-click menu, Item settings > Set item timebase to time
4. With all the midi items still selected: right-click > Source properties > OK > Ignore project tempo, use 120 BPM
5. Now you can freely change the Reaper project's tempo, and add tempo change events, without affecting the speed of the midi. Set the project's tempo to a value that musically matches the midi file. You will likely need to do a lot of trial and error, and use BPM numbers with two decimal places.

### Close Notes Silencing Each Other

If, when editing the song, you notice that notes right next to eachother seem to be silencing eachother, try zooming in very closely; you'll likely see a very small overlap between the two notes. Remove this overlap so the notes will play properly.

### Other

[Please do not attempt to use FL Studio to edit the midi files output by gbs2midi](https://gist.github.com/Thysbelon/a69da7038e65023a29168d9ef449acda).

## Credits
- This program uses [gbsplay](https://github.com/mmitch/gbsplay) to convert GBS files to a list of sound chip register writes, which my program then converts to a midi file.
- [libsmf from sseq2mid](https://github.com/Thysbelon/sseq2mid), originally written by [loveemu](https://github.com/loveemu/loveemu-lab/tree/master/nds/sseq2mid/src).
