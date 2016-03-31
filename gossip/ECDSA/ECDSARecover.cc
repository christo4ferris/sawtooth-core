// Copyright 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ------------------------------------------------------------------------------


/*
* @file	  ECDSARecover.cc
* @author Dan Middleton
* @date	  2016-01-28
* @status RESEARCH PROTOTYPE
*
* Recover public key from ECDSA Signature and associated message hash
* Given an ECDSA Signature: (r,s) and message hash, e
* Return public key, Q, as Q = r^-1(sr-eG)
* where G is the group Generator.
* Specifically written for secp256k1 curve with sha256. Should not be used with other curves or hash functions.
*/

#include "ECDSARecover.h"
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>

using namespace CryptoPP;
using namespace std;

/* Recovers the public key encoded in an ECDSA signature
 * @param msgHash: message hash;
 * @param r,s: signature pair
 * @param yBit: y recovery value as defined in Certicom Sec 1 v2.
 * @return Returns point Q (public key) as a serialized x,y pair.
 */
string recoverPubKeyFromSig(Integer e, Integer r, Integer s, int yBit) {
#ifdef DEBUG_PUBKRECOVER
    cout << endl << "Enter recoverPubKeyFromSig(...)" << endl;
#endif
    ECDSA<ECP, SHA256>::PrivateKey tmp;
    tmp.Initialize(ASN1::secp256k1(), 2); //use private key constructor to get the curve params

    //Setup variables   
    Integer h(tmp.GetGroupParameters().GetCofactor());
    Integer a(tmp.GetGroupParameters().GetCurve().GetA());
    Integer b(tmp.GetGroupParameters().GetCurve().GetB());
    Integer p(tmp.GetGroupParameters().GetCurve().GetField().GetModulus()); //Field modulus
    Integer n(tmp.GetGroupParameters().GetSubgroupOrder());                 //Curve modulus. #E(p)=n < #Fp=p
    ECPPoint G(tmp.GetGroupParameters().GetSubgroupGenerator());            //Curve generator
    ECPPoint R(1,1);                    //Point to be recovered from signature; initialized off curve for safety.
    Integer x(0L), y(0L), exp(0L);      //x, y, and exponentiation term used for recovering point R.

    ECP curve(p, a, b);	                //manually specify params for secp256k1 extracted from ECDSA class above. 

    if (r > n || r < 0) {               //Check inputs.
        string error = "Invalid signature. r exceeds group size.\n";
        throw std::domain_error(error);
        return "";
    }
    if (s > n || s < 0) {
        string error = "Invalid signature. s exceeds group size.\n";
        throw std::domain_error(error);
        return "";
    }
    if (e.BitCount() > 256 || e < 0) {   //e may be larger than n, but may not exceed sha256 bit length.
        string error = "Invalid signature. Message hash value out of range.\n";
        throw std::domain_error(error);
        return "";
    }

    //Use r (the x coordinate of R=kG) to compute y
    for (int i = 0; i < (h + 1); i++) { //Iterate over the cofactor to try multiple possible x deriving from r.
        x = r + i*n;                    //x may be between n and p and ~shrunken when set to r = x mod n.
        if (x>p) {
            string error = "Invalid signature. Recovered R.x exceeds field modulus.\n";
            throw std::domain_error(error);
            return "";                  //x could never have been larger than the field modulus, p.
        }
        
        y = (x * x * x + 7) % p;        //computes y^2 hardcoded to secp256k params a=0, b=7;
        exp = (p + 1) / 4;              //Exponentiation rule for finding sqrt when p = 3 mod 4 (see HAC 3.36)...
        y = a_exp_b_mod_c(y, exp, p);   //...find sqrt of y^2

        if ((yBit % 2) ^ (y % 2)) {     //yBit indicates if we expect y to be odd. If there's a mismatch then we need the other y.
            y = p - y;                  //sqrt(y^2) = {y,-y} if yBit trips then must select -y
        }

        R.x = x; R.y = y;
        if (curve.VerifyPoint(R)) {     //Check if this point is on the curve.
            break;                      //If so jump out of the cofactor loop
        }                               //If not maybe we have another loop iteration to find it.

    }


    if(!curve.VerifyPoint(R)){          //Validate final computed point is on the curve
        string error = "Recover Pub Key from Sig: Computed point is not on curve.\n";
        throw std::domain_error(error);
        return "";
    }

    //Compute Q=r^-1(sR-eG) mod p
    ECPPoint sR(curve.Multiply(s, R));  //compute s*R
    ECPPoint eG(curve.Multiply(e, G));  //compute e*G
    ECPPoint sR_eG(curve.Subtract(sR, eG));//compute sR-eG
    Integer rInv = r.InverseMod(n);     //Compute modular inverse of r
    ECPPoint Q(curve.Multiply(rInv, sR_eG));//Apply r_inverse to sR-eG

    /* 
     * Check that Q actually validates the message. For optimization this can probably be removed.
     * Crypto++ takes the message not a digest as input. We only have access to the digest.
     * i.e.: verifier.VerifyMessage((const byte*)message.data(), message.size(), (const byte*)signature.data(), signature.size());
     * Instead do signature verification from scratch.
     */
    //If Q or QP is the identity or if it isn't on the curve then fail
    if ((Q == curve.Identity()) || (curve.Multiply(p, Q) == curve.Identity()) || (!curve.VerifyPoint(Q))) {
        string error = "Recover Pub Key from Sig: Calculated Q fails basic criteria.\n";
        throw std::domain_error(error);
        return "";
    }

    //Compute ewG + rwQ; x component of sum should equal r for sig to verify
    Integer w(s.InverseMod(n));             //Calculate s^-1
    Integer u1(a_times_b_mod_c(e, w, n));   // u1 = ew mod n
    Integer u2(a_times_b_mod_c(r, w, n));   // u2 = rw mod n
    ECPPoint u1G(curve.Multiply(u1, G));    // u1*G
    ECPPoint u2Q(curve.Multiply(u2, Q));    // u2*Q
    ECPPoint X1(curve.Add(u1G, u2Q));       // u1G + u2Q; 
    if (!curve.VerifyPoint(X1)) { 
        string error = "x1 did not verify as a point on the curve.\n"; 
        throw std::domain_error(error);
        return ""; 
    }

    Integer x1 = X1.x % n;                  // take x coordinate mod n
    if (r != x1) {                          // if r == x1 then signature verifies
        string error = "Failed to recover pubkey from signature. Recovered key fails to verify signature\n";
        throw std::domain_error(error); 
        return "";
    }

#ifdef DEBUG_PUBKRECOVER
    cout << "Success recovering a pubkey from signature.\n";
    cout << "Computed R..." << endl;
    cout << "  R.x: " << R.x << endl;      
    cout << "  R.y: " << R.y << endl;
    cout << "Computed Q..." << endl;
    cout << "  Q.x: " << Q.x << endl;
    cout << "  Q.y: " << Q.y << endl;
    cout << "Q hex... " << endl;
    cout << "  Q.x: " << std::hex << Q.x << endl;
    cout << "  Q.y: " << Q.y << endl << std::dec;
    cout << "Input r:     " << r << endl;
    cout << "Computed x1: " << x1 << endl;
#endif

    std::stringstream xss, yss, stream;
    xss << std::hex << Q.x;                   //Get hex strings of points
    yss << std::hex << Q.y;
    string xstr = xss.str(); xstr.resize(xstr.size()-1); //  xstr.pop_back(); //Strip off cryptopp's hex "h" tag.
    string ystr = yss.str(); ystr.resize(ystr.size()-1); // ystr.pop_back();
    stream << std::setw(64) << std::setfill('0') << xstr; //Pad out 64 nibbles
    stream << std::setw(64) << std::setfill('0') << ystr; //Pad out 64 nibbles
    return stream.str();
}

// TEST method 
// Expects signature computed from the following
// d:2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7aeh
// k:48692452077975311141641379449682050563269990734773417387024709146437866544976 (note: dec)
// e:fcde2b2edba56bf408601fb721fe9b5c338d10ee429ea04fae5511b68fbf8fb9h
// Should have created an r,s:
// r:73822833206246044331228008262087004113076292229679808334250850393445001014761
// s:58995174607243353628346858794753620798088291196940745194581481841927132845752
void test(Integer e, Integer r, Integer s){
    ECDSA<ECP, SHA256>::PrivateKey tmp;
    tmp.Initialize(ASN1::secp256k1(), 2); //use private key constructor to get the curve params

    //Setup variables
    Integer h(tmp.GetGroupParameters().GetCofactor());
    Integer a(tmp.GetGroupParameters().GetCurve().GetA());
    Integer b(tmp.GetGroupParameters().GetCurve().GetB());
    Integer p(tmp.GetGroupParameters().GetCurve().GetField().GetModulus()); //Field modulus
    Integer n(tmp.GetGroupParameters().GetSubgroupOrder());                 //Curve modulus. #E(p)=n < #Fp=p
    ECPPoint G(tmp.GetGroupParameters().GetSubgroupGenerator());            //Curve generator
    ECP curve(p, a, b); //manually specify params for secp256k extracted from ECDSA class above.
    Integer d("2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7aeh");

    //derive k
    Integer k("48692452077975311141641379449682050563269990734773417387024709146437866544976"); //yanked from python
    Integer w = s.InverseMod(n);
    cout << "TEST: Expected k: " << k << endl;
    ECPPoint RPrime(curve.Multiply(k,G));
    Integer rx = RPrime.x %n;
    Integer ry = RPrime.y %n;
    cout << "TEST: R computed from k\n";
    cout << "TEST: kG.x mod n: " << rx << endl; 
    cout << "TEST: kG.y mod n: " << ry << endl;
    k = 0;
    cout << "TEST: Cleared k: " << k << endl;
    k = (e + r*d) %n;
    k = w * k %n;
    ECPPoint R(curve.Multiply(k, G));
    if(r == R.x) {
        cout << "TEST: k verified by r==R.x\n" << "TEST: k: " << k << endl;
    } else {
        cerr << "TEST: k computation FAILED\n" << "TEST: k: " << k << endl;
    }
    cout << "TEST: computed R.x: " << R.x << endl;

    //Derive e = sk - rd 
    Integer u = s * k % n;
    Integer v = r * d % n;
    v = n - v; 
    Integer derived_e = u + v %n;
    if(e == derived_e) {
        cout << "TEST: e verified by sk-rd\n" << "TEST: e': " << derived_e << endl;
    } else {
        cerr << "TEST: e compuation FAILED\n" << "TEST: e': " << derived_e << endl;
    }
}

string recoverPubKeyFromSig(string msgHash, string sig_r, string sig_s, int yBit) {
    if (msgHash.empty() || sig_r.empty() || sig_s.empty() || yBit > 3 || yBit < 0)
        throw std::invalid_argument("Empty string or invalid yBit value.\n");
    try {
        Integer e(msgHash.data());
        Integer r(sig_r.data());
        Integer s(sig_s.data());
#ifdef DEBUG_PUBKRECOVER
        cout << "In c++ code" << endl;
        cout << "e:      " << e << endl;
        cout << "hex(e): " << std::hex << e << endl;
        cout << "r:      " << std::dec << r << endl;
        cout << "s:      " << s << endl;
        cout << "ybit:   " << yBit << endl;
#endif
#ifdef TEST_PUBKRECOVER
        test(e, r, s);
#endif
        return recoverPubKeyFromSig(e, r, s, yBit);
    }
    catch (std::domain_error e) {
        throw(e);
        return "";
    }
    catch (exception e) {
        throw(e);
        return "";
    }
}

string recoverPubKeyFromSig_Base32(string msgHash, string sig_r, string sig_s, int yBit) {
    Integer e, r, s;
    byte tmp[32];
    word64 size;

    Base32Decoder decoderA;

    decoderA.Put((byte*)msgHash.data(), msgHash.size());
    decoderA.MessageEnd();
    size = decoderA.MaxRetrievable();

    if (size && size <= SIZE_MAX)
    {
        decoderA.Get(tmp, 32);
        e.Decode(tmp, 32);
        cout << "decoded e: " << e << endl;
    }
    else {
        string error = "Invalid sized msg hash to recoverPubkeyFromSig\n";
        throw std::invalid_argument(error);
        return "";
    }
    //decoder.Initialize();
    Base32Decoder decoderB;
    decoderB.Put((byte*)sig_r.data(), sig_r.size());
    decoderB.MessageEnd();
    size = decoderB.MaxRetrievable();
    if (size && size <= SIZE_MAX)
    {
        decoderB.Get(tmp, 32);
        r.Decode(tmp, 32);
        cout << "decoded r: " << r << endl;
    }
    else {
        string error = "Invalid sized sig_r to recoverPubkeyFromSig\n";
        throw std::invalid_argument(error);
        return "";
    }
    //decoder.Initialize();
    Base32Decoder decoderC;
    decoderC.Put((byte*)sig_s.data(), sig_s.size());
    decoderC.MessageEnd();
    size = decoderC.MaxRetrievable();
    if (size && size <= SIZE_MAX)
    {
        decoderC.Get(tmp, 32);
        s.Decode(tmp, 32);
        cout << "decoded s: " << s << endl;
    }
    else {
        string error = "Invalid sized sig_s to recoverPubkeyFromSig\n";
        throw std::invalid_argument(error);
        return "";
    }
//TODO: Pulled the base32 return format out of the main recovery function.  Need to do that here.  Something like this:
/*  byte buffer[64];
    Q.x.Encode(&buffer[0], 32);
    Q.y.Encode(&buffer[32], 32);

    Base32Encoder encoder(NULL, false);
    encoder.Put(buffer, 64);
    encoder.MessageEnd();

    string encoded;
    encoded.resize(encoder.MaxRetrievable());
    encoder.Get((byte *)encoded.data(), encoded.size());
*/
    return recoverPubKeyFromSig(e, r, s, yBit);

}