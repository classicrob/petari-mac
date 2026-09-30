#!/usr/bin/env python3
import csv
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'native/tools'))
from recorded_route import convert

class RecorderTests(unittest.TestCase):
    def test_writer_background_csv_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory)
            source=d/'test.cpp'
            source.write_text(r'''
#include "route_record_writer.hpp"
#include <cassert>
int main(int argc,char** argv) {
    using namespace PetariNative::App;
    RouteRecordWriter background;
    assert(!background.open(argv[1],true));
    {
        RouteRecordWriter writer;
        assert(writer.open(argv[2],false));
        RouteRecordFrame row; row.stage="A,\"B"; row.valid=true; row.gy=-2;
        row.input.jump=true; row.input.spin=true; row.input.moveX=.5f;
        writer.write(row);
    }
    RouteRecordWriter duplicate;
    assert(!duplicate.open(argv[2],false));
}
''')
            subprocess.run(['c++','-std=c++17','-fsanitize=undefined','-I'+str(ROOT/'native/app'),
                            '-I'+str(ROOT/'native/include'),str(source),'-o',str(d/'test')],check=True)
            subprocess.run([str(d/'test'),str(d/'background.csv'),str(d/'route.csv')],check=True)
            self.assertFalse((d/'background.csv').exists())
            rows=list(csv.DictReader((d/'route.csv').open()))
            self.assertEqual(len(rows),1)
            self.assertEqual(rows[0]['stage'],'A,"B')
            self.assertEqual(rows[0]['up_y'],'1.000000')
            self.assertEqual(rows[0]['spin'],'1')
            self.assertEqual(rows[0]['jump'],'1')
            self.assertNotIn(None,rows[0])

    def test_jump_spin_landing_and_multiple_visits(self):
        def row(frame,x,y,ground,jump=0,spin=0):
            return dict(frame=str(frame),stage='AstroGalaxy',valid='1',x=str(x),y=str(y),z='0',
                        grounded=str(ground),jump=str(jump),spin=str(spin))
        rows=[row(0,0,0,1),row(1,10,0,1,1),row(2,20,100,0),row(3,30,150,0,0,1),row(4,40,0,1)]
        points=convert(rows)
        self.assertIn((0.,0.,0.,'Hop'),points)
        self.assertIn((30.,150.,0.,'Spin'),points)
        self.assertEqual(points[-1],(40.,0.,0.,'Walk'))
        with self.assertRaises(ValueError): convert(rows+[dict(rows[0],stage='Other')]+rows)

    def test_wall_kick_and_launch_star(self):
        def row(frame,x,y,ground,jump=0,spin=0,bound=0):
            return dict(frame=str(frame),stage='AstroGalaxy',valid='1',x=str(x),y=str(y),z='0',
                        grounded=str(ground),jump=str(jump),spin=str(spin),bound=str(bound))
        rows=[row(0,0,0,1),row(1,0,20,0,1),row(2,5,100,0),row(3,5,100,0,1),row(4,50,200,0),
              row(5,90,200,0,1),row(6,90,300,0),row(7,100,300,1),row(8,110,300,1),
              row(9,120,310,0,0,0,1),row(10,130,320,0,0,1,1),row(11,2000,320,0,0,0,1),row(12,2000,300,1)]
        points=convert(rows)
        self.assertEqual([a for *_,a in points if a!='Walk'],['Hop','Kick','Kick','Launch'])
        self.assertIn((5.,100.,0.,'Kick'),points)
        self.assertIn((110.,300.,0.,'Launch'),points)
        self.assertNotIn('Spin',[a for *_,a in points])
        warp=[dict(r,spin='0') for r in rows[6:]]
        self.assertIn((110.,300.,0.,'Warp'),convert(warp))
        with self.assertRaises(ValueError): convert([])

if __name__=='__main__':unittest.main()
