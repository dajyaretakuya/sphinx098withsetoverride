#define _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS

#include "SegmenterManager.h"
#include "Segmenter.h"

#include "sphinx.h"
#include "tokenizer_zhcn.h"
#if USE_LIBICONV
#include <iconv.h>
#endif

#if USE_WINDOWS
#define ICONV_INBUF_CONST	1
#endif

////////////////////////////////////////////////////////////
typedef CSR_Singleton<css::SegmenterManager> SegmenterManagerSingleInstance;

class CSphTokenizer_zh_CN_UTF8_Private
{
public:
	CSphTokenizer_zh_CN_UTF8_Private()
		:m_seg(NULL), m_mgr(NULL) {
#if USE_LIBICONV
		m_iconv = NULL;
#endif
		if(!m_lower)
			m_lower = css::ToLower::Get();
		if(!m_tagger)
			m_tagger = css::ChineseCharTagger::Get();
	}
	
	~CSphTokenizer_zh_CN_UTF8_Private() {
#if USE_LIBICONV
		if(m_iconv)
			iconv_close(m_iconv); 
		m_iconv = NULL;
#endif
	};
	
	css::Segmenter* GetSegmenter(const char* dict_path) {
		int nRet = 0;
		if(!m_mgr) {
			m_mgr = SegmenterManagerSingleInstance::Get();
			if(dict_path)
				nRet = m_mgr->init(dict_path);
		}
		if(nRet == 0 && !m_seg) 
			m_seg = m_mgr->getSegmenter();
		return m_seg;
	}
#if USE_LIBICONV	
	iconv_t GetConverter(const char* from, const char* to) {
		if(m_iconv)
			return m_iconv;
		//m_iconv = iconv_open ("UTF-8//IGNORE", "GB18030");
		m_iconv = iconv_open (to, from);
		if (m_iconv == (iconv_t) -1) //error check.
			return (iconv_t)(-1);
		iconv(m_iconv, NULL, NULL, NULL, NULL);
#if 0
		//ignore invalid char-seq
		int one = 1;
		iconvctl(m_iconv, ICONV_SET_DISCARD_ILSEQ, &one); 
#endif
		return m_iconv;
	}
#endif

public:
	static css::ToLowerImpl* m_lower;
	static css::ChineseCharTaggerImpl* m_tagger;
protected:
	css::Segmenter* m_seg;
	css::SegmenterManager* m_mgr;
#if USE_LIBICONV
	iconv_t m_iconv;
#endif
};

css::ToLowerImpl* CSphTokenizer_zh_CN_UTF8_Private::m_lower = NULL;
css::ChineseCharTaggerImpl* CSphTokenizer_zh_CN_UTF8_Private::m_tagger = NULL;

////////////////////////////////////////////////////////////
CSphTokenizer_zh_CN_UTF8::CSphTokenizer_zh_CN_UTF8 ()
{
	m_dictpath = NULL;
	d_ = new CSphTokenizer_zh_CN_UTF8_Private();
	m_tLC.Reset();
}

CSphTokenizer_zh_CN_UTF8::~CSphTokenizer_zh_CN_UTF8 ()
{
	delete d_;
}

void CSphTokenizer_zh_CN_UTF8::SetBuffer ( BYTE * sBuffer, int iLength )
{
	// check that old one is over and that new length is sane
	assert ( iLength>=0 );

	// set buffer
	m_pBuffer = sBuffer;
	m_pBufferMax = sBuffer + iLength;
	m_pCur = sBuffer;

	// fixup embedded zeroes with spaces
	for ( BYTE * p = m_pBuffer; p < m_pBufferMax; p++ )
		if ( !*p )
			*p = ' ';

	m_iOvershortCount = 0;
	//real set buffer.
	css::Segmenter* seg = d_->GetSegmenter(m_dictpath);
	seg->setBuffer((u1*)m_pBuffer,m_pBufferMax - m_pBuffer);
	//be ready for getToekn
	FlushAccum ();
}

void CSphTokenizer_zh_CN_UTF8::FlushAccum ()
{
	assert ( m_pAccum-m_sAccum < (int)sizeof(m_sAccum) );
	m_iLastTokenLen = m_iAccum;
	*m_pAccum = 0;
	m_iAccum = 0;
	m_pAccum = m_sAccum;
}

BYTE * CSphTokenizer_zh_CN_UTF8::GetToken ()
{
	m_bWasSpecial = false;
	css::Segmenter* seg = d_->GetSegmenter(m_dictpath); //TODO fill blank here
	//seg->setBuffer((u1*)m_pBuffer,m_pBufferMax - m_pBuffer);
	u2 len = 0, symlen = 0;
	const char* tok = NULL;
	
	do {
		tok = (const char*)seg->peekToken(len, symlen);
		seg->popToken(len);
	}while(tok && tok<(const char*)m_pBufferMax &&tok<(const char*)m_pCur);
	
	if(len > 3*SPH_MAX_WORD_LEN)
		len = 3*SPH_MAX_WORD_LEN; //this might cause a wrong token. no token can larger than 64-char
	//FIXME: NO Memcpy, 1. ToLower 2. Exceptions[done in libmmseg] 3. eat blanks
	//to Lower
	BYTE* tok_ptr = (BYTE*)tok;
	BYTE* tok_max = tok_ptr + symlen;
	m_pTokenStart = tok_ptr;
	m_pTokenEnd = tok_max;
	m_bTokenBoundary = false;

	while ( tok_ptr<tok_max )
	{
		int iCode = sphUTF8Decode ( tok_ptr );
		if ( iCode>=0 ){
			iCode = (int)d_->m_lower->toLower((u2)iCode);
			int iFolded = m_tLC.ToLower ( iCode );
			if ( ( iFolded & FLAG_CODEPOINT_SPECIAL ) && m_iAccum==0 )
			{
				m_bWasSpecial = true;

				AccumCodepoint ( iFolded & MASK_CODEPOINT );
				*m_pAccum = '\0';
				
				//m_pTokenStart = tok_ptr;
				//m_pTokenEnd = m_pCur;
				m_pCur =  tok_ptr;
				m_iLastTokenLen = 1;
				FlushAccum ();

				return m_sAccum;
			}
			//check is sep, no sep can be passed to user
			//if(IsSeparator (iFolded,  m_iAccum==0))
			u2 tag = d_->m_tagger->tagUnicode(iCode,1);
			tag = (tag&0x3F) + 'a' -1;
			if(tag == 'w' || iCode == ' ')
			{
				//skip the 1st char
				m_bBoundary = true;
			}
			else
			{
				m_bBoundary = false;
				if ( m_iAccum == 0 )
					m_pTokenStart = (BYTE*)tok;

				AccumCodepoint ( iCode );
			}
			//AccumCodepoint ( iCode ); //will cut token is larger than SPH_MAX_WORD_LEN
		}else {
			break;
		}
	}
	if(m_bBoundary)
		m_bTokenBoundary = true;
	//?? m_pCur =  tok_ptr;
	FlushAccum ();

	//memcpy(m_sAccum, tok, len);
	//m_sAccum[symlen] = 0;
	//printf("%*.*s/x ",symlen,symlen,tok);
	if(m_sAccum[0] == '\r' || m_sAccum[0] == '\n') {
		//m_sAccum[0] = 0;
		return GetToken();
	}
	//printf("%s,",m_sA^ccum);
	if(symlen){
		//m_pTokenStart = m_sAccum;
		//m_pTokenEnd = &m_sAccum[symlen];
		return m_sAccum;
	}else{
		//printf("\n");
		return NULL;
	}
}


ISphTokenizer *		CSphTokenizer_zh_CN_UTF8::Clone ( bool bEscaped ) const
{
	CSphTokenizer_zh_CN_UTF8 * pClone = new CSphTokenizer_zh_CN_UTF8 ();
	pClone->CloneBase ( this, bEscaped );
	pClone->m_dictpath = m_dictpath;
	return pClone;
}


int CSphTokenizer_zh_CN_UTF8::GetCodepointLength ( int iCode ) const
{
	if ( iCode<128 )
		return 1;

	int iBytes = 0;
	while ( iCode & 0x80 )
	{
		iBytes++;
		iCode <<= 1;
	}

	assert ( iBytes>=2 && iBytes<=4 );
	return iBytes;
}

#if USE_LIBICONV

CSphTokenizer_zh_CN_GBK::CSphTokenizer_zh_CN_GBK ()
	:m_convert_buffer(NULL)
{
	m_convert_buffer = m_default_convert_buffer;
	m_buffer_size = GBK_CONVERT_BUFFER_SIZE;
}

CSphTokenizer_zh_CN_GBK::~CSphTokenizer_zh_CN_GBK()
{
	if(m_convert_buffer != m_default_convert_buffer)
		free(m_convert_buffer);
	m_convert_buffer = NULL;
}

void CSphTokenizer_zh_CN_GBK::SetBuffer( BYTE * sBuffer, int iLength )
{
	//check convert buffer
	int tLength = (int)(iLength*1.5+3);
	if(tLength > m_buffer_size){
		if(m_convert_buffer != m_default_convert_buffer)
			free(m_convert_buffer);
		m_convert_buffer = (BYTE*)malloc(tLength);
		m_buffer_size = tLength;
	}
	//convert
	iconv_t it = d_->GetConverter("GB18030", "UTF-8//IGNORE");
	
#if ICONV_INBUF_CONST
	const char * ptr = (char*)sBuffer;
#else
	char * ptr = (char*)sBuffer;
#endif

	char * target_ptr = (char*)m_convert_buffer;
	size_t inbytes_remaining = (size_t)iLength;
	size_t outbytes_remaining = (size_t)m_buffer_size;
	int err = iconv (it, &ptr, &inbytes_remaining, &target_ptr, &outbytes_remaining);
	if(err>=0)
		*target_ptr = 0; //end the char
	//call base::setbuffer
	CSphTokenizer_zh_CN_UTF8::SetBuffer(m_convert_buffer, m_buffer_size - outbytes_remaining);
}

/*
BYTE *	CSphTokenizer_zh_CN_GBK::GetToken ()
{
	return NULL;
}
*/


ISphTokenizer *	CSphTokenizer_zh_CN_GBK::Clone ( bool bEscaped ) const
{
	CSphTokenizer_zh_CN_GBK * pClone = new CSphTokenizer_zh_CN_GBK ();
	pClone->CloneBase ( this, bEscaped );
	pClone->m_dictpath = m_dictpath;
	return pClone;
}

#endif

